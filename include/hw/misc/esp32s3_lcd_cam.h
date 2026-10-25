/*
 * ESP32-S3 LCD_CAM, camera (DVP receiver) side
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/dma/esp_gdma.h"
#include "qemu/timer.h"

#define TYPE_ESP32S3_LCD_CAM "esp32s3.lcd_cam"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3LcdCamState, ESP32S3_LCD_CAM)

#define ESP32S3_LCD_CAM_IO_SIZE 0x100

REG32(LCD_CAM_CAM_CTRL, 0x04)
    FIELD(LCD_CAM_CAM_CTRL, CAM_UPDATE, 4, 1)
REG32(LCD_CAM_CAM_CTRL1, 0x08)
    FIELD(LCD_CAM_CAM_CTRL1, REC_DATA_BYTELEN, 0, 16)
    FIELD(LCD_CAM_CAM_CTRL1, CAM_START, 29, 1)
    FIELD(LCD_CAM_CAM_CTRL1, CAM_RESET, 30, 1)
    FIELD(LCD_CAM_CAM_CTRL1, CAM_AFIFO_RESET, 31, 1)
REG32(LCD_CAM_LC_DMA_INT_ENA, 0x64)
    FIELD(LCD_CAM_LC_DMA_INT, CAM_VSYNC, 2, 1)
REG32(LCD_CAM_LC_DMA_INT_RAW, 0x68)
REG32(LCD_CAM_LC_DMA_INT_ST, 0x6c)
REG32(LCD_CAM_LC_DMA_INT_CLR, 0x70)
REG32(LCD_CAM_LC_REG_DATE, 0xfc)

struct ESP32S3LcdCamState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    ESPGdmaState *gdma;
    /* Time between two frames of the sensor */
    uint32_t frame_interval_ms;
    /* Time to receive one EOF-terminated chunk of a frame */
    uint32_t chunk_interval_us;

    QEMUTimer frame_timer;
    QEMUTimer chunk_timer;
    /* The frame being received, and how much of it the GDMA has */
    GByteArray *frame;
    uint32_t frame_pos;

    uint32_t int_raw;
    uint32_t int_ena;
    uint32_t regs[ESP32S3_LCD_CAM_IO_SIZE / 4];
};
