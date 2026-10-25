/*
 * ESP32-S3 LCD_CAM, camera (DVP receiver) side
 *
 * The sensor runs freely: every frame interval it ends a frame with VSYNC.
 * If the camera was started (CAM_START) and the GDMA IN channel selected
 * for LCD_CAM has a running link, the frame before that VSYNC is the next
 * frame of the machine's DVP sensor (see dvp_sensor.h). Its bytes reach
 * the link in chunks of CAM_REC_DATA_BYTELEN + 1, one per chunk interval,
 * each ending a GDMA transfer (IN_SUC_EOF), as with CAM_VS_EOF_EN clear.
 * The rest of the last, partial chunk is zero-filled without an EOF:
 * esp32-camera copies that whole chunk at VSYNC and looks for the JPEG EOI
 * from its end, so stale bytes there must not hold an old EOI.
 *
 * The LCD side and the YUV converter are not emulated.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/misc/esp32s3_lcd_cam.h"
#include "hw/misc/dvp_sensor.h"
#include "trace.h"

static void esp32s3_lcd_cam_update_irq(ESP32S3LcdCamState *s)
{
    qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
}

static void esp32s3_lcd_cam_vsync(ESP32S3LcdCamState *s)
{
    s->int_raw |= R_LCD_CAM_LC_DMA_INT_CAM_VSYNC_MASK;
    esp32s3_lcd_cam_update_irq(s);
}

static void esp32s3_lcd_cam_abort(ESP32S3LcdCamState *s)
{
    timer_del(&s->chunk_timer);
    if (s->frame) {
        g_byte_array_unref(s->frame);
        s->frame = NULL;
    }
}

static bool esp32s3_lcd_cam_dma_channel(ESP32S3LcdCamState *s, uint32_t *chan)
{
    return esp_gdma_get_in_channel(s->gdma, GDMA_LCDCAM, chan) &&
           s->gdma->ch_conf[ESP_GDMA_IN_IDX][*chan].stream_running;
}

/* Hand the GDMA the next chunk of the frame; end the frame after the last */
static void esp32s3_lcd_cam_chunk(void *opaque)
{
    ESP32S3LcdCamState *s = opaque;
    const uint32_t chunk = FIELD_EX32(s->regs[R_LCD_CAM_CAM_CTRL1],
                                      LCD_CAM_CAM_CTRL1, REC_DATA_BYTELEN) + 1;
    const uint32_t left = s->frame->len - s->frame_pos;
    uint32_t chan;
    bool ok = false;

    if (esp32s3_lcd_cam_dma_channel(s, &chan)) {
        if (left >= chunk) {
            ok = esp_gdma_push_in(s->gdma, chan, s->frame->data + s->frame_pos,
                                  chunk, true);
        } else {
            /* The partial last chunk, padded to a whole one with zeros */
            g_autofree uint8_t *last = g_malloc0(chunk);
            memcpy(last, s->frame->data + s->frame_pos, left);
            ok = esp_gdma_push_in(s->gdma, chan, last, chunk, false);
        }
    }
    s->frame_pos += MIN(left, chunk);

    /* After a whole chunk there is more, if only the zeros after the EOI */
    if (ok && left >= chunk) {
        timer_mod(&s->chunk_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
                  s->chunk_interval_us);
        return;
    }
    trace_esp32s3_lcd_cam_frame_end(s->frame_pos, ok);
    esp32s3_lcd_cam_abort(s);
    esp32s3_lcd_cam_vsync(s);
}

static void esp32s3_lcd_cam_frame(void *opaque)
{
    ESP32S3LcdCamState *s = opaque;
    Object *sensor = NULL;
    uint32_t chan;

    timer_mod(&s->frame_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->frame_interval_ms);
    if (s->frame) {
        /* Still receiving the previous frame */
        return;
    }
    if (FIELD_EX32(s->regs[R_LCD_CAM_CAM_CTRL1], LCD_CAM_CAM_CTRL1,
                   CAM_START) && esp32s3_lcd_cam_dma_channel(s, &chan)) {
        sensor = dvp_sensor_find();
    }
    if (sensor) {
        s->frame = DVP_SENSOR_GET_CLASS(sensor)->next_frame(sensor);
    }
    if (!s->frame) {
        esp32s3_lcd_cam_vsync(s);
        return;
    }
    trace_esp32s3_lcd_cam_frame_start(s->frame->len);
    s->frame_pos = 0;
    esp32s3_lcd_cam_chunk(s);
}

static uint64_t esp32s3_lcd_cam_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(opaque);

    switch (addr) {
    case A_LCD_CAM_LC_DMA_INT_ENA:
        return s->int_ena;
    case A_LCD_CAM_LC_DMA_INT_RAW:
        return s->int_raw;
    case A_LCD_CAM_LC_DMA_INT_ST:
        return s->int_raw & s->int_ena;
    case A_LCD_CAM_LC_DMA_INT_CLR:
        return 0;
    default:
        return s->regs[addr / 4];
    }
}

static void esp32s3_lcd_cam_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned int size)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(opaque);

    switch (addr) {
    case A_LCD_CAM_CAM_CTRL:
        s->regs[addr / 4] = value & ~R_LCD_CAM_CAM_CTRL_CAM_UPDATE_MASK;
        break;
    case A_LCD_CAM_CAM_CTRL1:
        if (value & R_LCD_CAM_CAM_CTRL1_CAM_RESET_MASK) {
            esp32s3_lcd_cam_abort(s);
        }
        s->regs[addr / 4] = value & ~(R_LCD_CAM_CAM_CTRL1_CAM_RESET_MASK |
                                      R_LCD_CAM_CAM_CTRL1_CAM_AFIFO_RESET_MASK);
        break;
    case A_LCD_CAM_LC_DMA_INT_ENA:
        s->int_ena = value;
        esp32s3_lcd_cam_update_irq(s);
        break;
    case A_LCD_CAM_LC_DMA_INT_CLR:
        s->int_raw &= ~value;
        esp32s3_lcd_cam_update_irq(s);
        break;
    case A_LCD_CAM_LC_DMA_INT_RAW:
    case A_LCD_CAM_LC_DMA_INT_ST:
    case A_LCD_CAM_LC_REG_DATE:
        break;
    default:
        s->regs[addr / 4] = value;
        break;
    }
}

static const MemoryRegionOps esp32s3_lcd_cam_ops = {
    .read = esp32s3_lcd_cam_read,
    .write = esp32s3_lcd_cam_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void esp32s3_lcd_cam_reset_hold(Object *obj, ResetType type)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(obj);

    esp32s3_lcd_cam_abort(s);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[A_LCD_CAM_LC_REG_DATE / 4] = 0x2010080;
    s->int_raw = 0;
    s->int_ena = 0;
    qemu_irq_lower(s->irq);
}

static void esp32s3_lcd_cam_realize(DeviceState *dev, Error **errp)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(dev);

    if (!s->gdma) {
        error_setg(errp, "%s: gdma is not set", TYPE_ESP32S3_LCD_CAM);
        return;
    }
    timer_mod(&s->frame_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->frame_interval_ms);
}

static void esp32s3_lcd_cam_init(Object *obj)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_lcd_cam_ops, s,
                          TYPE_ESP32S3_LCD_CAM, ESP32S3_LCD_CAM_IO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    timer_init_ms(&s->frame_timer, QEMU_CLOCK_VIRTUAL,
                  esp32s3_lcd_cam_frame, s);
    timer_init_us(&s->chunk_timer, QEMU_CLOCK_VIRTUAL,
                  esp32s3_lcd_cam_chunk, s);
}

static Property esp32s3_lcd_cam_properties[] = {
    DEFINE_PROP_LINK("gdma", ESP32S3LcdCamState, gdma, TYPE_ESP_GDMA,
                     ESPGdmaState *),
    DEFINE_PROP_UINT32("frame-interval-ms", ESP32S3LcdCamState,
                       frame_interval_ms, 66),
    DEFINE_PROP_UINT32("chunk-interval-us", ESP32S3LcdCamState,
                       chunk_interval_us, 500),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_lcd_cam_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = esp32s3_lcd_cam_realize;
    rc->phases.hold = esp32s3_lcd_cam_reset_hold;
    device_class_set_props(dc, esp32s3_lcd_cam_properties);
}

static const TypeInfo esp32s3_lcd_cam_info = {
    .name = TYPE_ESP32S3_LCD_CAM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3LcdCamState),
    .instance_init = esp32s3_lcd_cam_init,
    .class_init = esp32s3_lcd_cam_class_init,
};

static void esp32s3_lcd_cam_register_types(void)
{
    type_register_static(&esp32s3_lcd_cam_info);
}

type_init(esp32s3_lcd_cam_register_types)
