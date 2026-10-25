# Camera emulation for ESP32-S3

The `esp32s3` machine can emulate a DVP camera, so that an unmodified
ESP-IDF application using [esp32-camera](https://github.com/espressif/esp32-camera)
captures JPEG frames in QEMU: the driver finds the sensor over SCCB (I2C),
configures it, and receives its frames through LCD_CAM and the GDMA as on
the chip.

Tested with esp32-camera 2.0 on ESP-IDF 5.5, in JPEG mode with frame buffers
in PSRAM and an XCLK other than 16 MHz, where the GDMA fills a ring of
internal-RAM buffers. Its other mode (XCLK at 16 MHz, the GDMA writing into
PSRAM directly) uses the same paths but has not been run.

## Quick start

An OV5640 on I2C1, with 8 MB of octal PSRAM for the frame buffers:

```
qemu-system-xtensa -nographic -machine esp32s3 -m 8M \
    -global driver=ssi_psram,property=is_octal,value=true \
    -device ov5640,bus=i2c1,address=0x3c \
    -drive file=flash.bin,if=mtd,format=raw
```

- Put the sensor on the I2C controller the application uses for SCCB:
  `i2c0` or `i2c1` (esp32-camera: `CONFIG_SCCB_HARDWARE_I2C_PORT0/1`). The pins
  do not matter.
- PSRAM is optional for the camera itself, but esp32-camera's default
  `CAMERA_FB_IN_PSRAM` needs it. `-m` gives its size; octal PSRAM, the
  `CONFIG_SPIRAM_MODE_OCT` of most ESP32-S3 modules, needs `is_octal`.

## What the guest sees

| Part | Model | Notes |
| --- | --- | --- |
| I2C0, I2C1 (`0x60013000`, `0x60027000`) | `esp32s3.i2c` | Master mode, the 8-command list of the S3 (opcodes differ from the ESP32's). Runs at `TRANS_START`, no bus timing. Buses `i2c0` and `i2c1`. |
| OV5640 | `ov5640` | SCCB register file (16-bit addresses), chip ID `0x5640`, software reset. JPEG output only. |
| LCD_CAM (`0x60041000`) | `esp32s3.lcd_cam` | Camera side only: VSYNC interrupt, data to the GDMA channel with `PERI_SEL` LCD_CAM. No LCD, no YUV converter. |
| GDMA | `esp32s3.gdma` | Peripheral-to-memory streaming into an IN link (`esp_gdma_push_in()`), and access to PSRAM. |

All three reset through `SYSTEM_PERIP_RST_EN0/1`, as ESP-IDF expects at boot.

## Frames

The sensor runs freely: LCD_CAM ends a frame with VSYNC every
`frame-interval-ms` (default 66, about 15 fps). When the guest has started the
camera (`CAM_START`) and the GDMA link, that frame carries the sensor's next
image, pushed into the link in chunks of `CAM_REC_DATA_BYTELEN + 1` bytes, one
every `chunk-interval-us` (default 500), each ending with `IN_SUC_EOF`. The last
partial chunk is zero-filled and has no EOF, then VSYNC follows. Both
intervals are properties of `esp32s3.lcd_cam`, for example
`-global driver=esp32s3.lcd_cam,property=frame-interval-ms,value=33`.

The OV5640's image is colour bars over a vertical gradient, encoded as a
baseline JPEG (4:2:0) with no dependency on libjpeg:

- its size is the output size the driver programmed (`0x3808`-`0x380b`);
- its quality follows the JPEG quantization scale `0x4407[5:0]`, from 100 at
  0 to 5 at 63, so a lower esp32-camera `jpeg_quality` gives a larger file;
- it is produced only while the sensor is powered up (`0x3008[6]` clear) in
  JPEG mode (`0x3821[5]`), and encoded again only when a setting changes.

A frame decodes with any JPEG decoder and carries the comment `QEMU OV5640`.

## Other sensors

LCD_CAM takes frames from whichever device implements the `dvp-sensor`
interface (`include/hw/misc/dvp_sensor.h`): `next_frame()` returns the bytes the
sensor puts on its data lines between two VSYNCs. A model for another sensor
adds its SCCB registers and implements that method.

## Debugging

Trace events: `ov5640_write` (each SCCB register write), `ov5640_frame` (each
new encoding), `esp32s3_lcd_cam_frame_start` and `esp32s3_lcd_cam_frame_end`.
For example `-trace 'esp32s3_lcd_cam*'`, or over QMP:
`{"execute": "trace-event-set-state", "arguments": {"name": "ov5640*", "enable": true}}`.

If esp32-camera reports `Failed to get the frame on time!`, no frame reached
the driver: check that LCD_CAM has `CAM_START` and that the GDMA IN channel
with `PERI_SEL` 5 has a running link (`xp` on `0x60041008` and the channel's
`IN_LINK`).

## Known limits

- JPEG only: in RGB565, YUV or grayscale mode the sensor produces no frames.
- No line or HSYNC interrupts, no `CAM_VS_EOF_EN` (EOF from VSYNC) mode.
- Timing is nominal: the pixel clock and PLL settings are stored, not used.
