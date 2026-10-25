/*
 * OmniVision OV5640 camera sensor, JPEG output only
 *
 * The SCCB (I2C) side is a 64 KiB register file with 16-bit, big-endian
 * register addresses and auto-increment, holding the chip ID and the
 * settings a driver reads back. Of those settings, the model uses:
 * - SYSTEM_CTROL0 (0x3008): bit 7 software reset, bit 6 power down;
 * - TIMING_TC_REG21 (0x3821) bit 5: JPEG output;
 * - X/Y_OUTPUT_SIZE (0x3808-0x380b): the frame size;
 * - COMPRESSION_CTRL07 (0x4407) bits 5:0: the JPEG quantization scale,
 *   lower is better.
 * While powered up in JPEG mode, each frame is a colour-bar JPEG of the
 * output size, at a quality that follows the quantization scale.
 *
 * Create with "-device ov5640,bus=<i2c bus>,address=0x3c".
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/cam_jpeg.h"
#include "hw/misc/dvp_sensor.h"
#include "trace.h"

#define TYPE_OV5640 "ov5640"
OBJECT_DECLARE_SIMPLE_TYPE(OV5640State, OV5640)

#define OV5640_SYSTEM_CTROL0    0x3008
#define OV5640_SOFT_RESET       BIT(7)
#define OV5640_POWER_DOWN       BIT(6)
#define OV5640_CHIP_ID_H        0x300a
#define OV5640_CHIP_ID_L        0x300b
#define OV5640_X_OUTPUT_SIZE    0x3808
#define OV5640_Y_OUTPUT_SIZE    0x380a
#define OV5640_TIMING_TC_REG21  0x3821
#define OV5640_JPEG_EN          BIT(5)
#define OV5640_COMPRESSION_07   0x4407

struct OV5640State {
    I2CSlave parent_obj;

    uint8_t regs[0x10000];
    uint16_t addr;
    /* Register address bytes still expected at the start of a write */
    int addr_bytes;

    /* The last frame and the settings it was made with */
    GByteArray *frame;
    unsigned frame_width;
    unsigned frame_height;
    int frame_quality;
};

static void ov5640_reset_regs(OV5640State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[OV5640_CHIP_ID_H] = 0x56;
    s->regs[OV5640_CHIP_ID_L] = 0x40;
    /* Full 2592x1944 output, quantization scale 4 */
    s->regs[OV5640_X_OUTPUT_SIZE] = 0x0a;
    s->regs[OV5640_X_OUTPUT_SIZE + 1] = 0x20;
    s->regs[OV5640_Y_OUTPUT_SIZE] = 0x07;
    s->regs[OV5640_Y_OUTPUT_SIZE + 1] = 0x98;
    s->regs[OV5640_COMPRESSION_07] = 0x04;
}

static void ov5640_write_reg(OV5640State *s, uint16_t reg, uint8_t value)
{
    trace_ov5640_write(reg, value);
    if (reg == OV5640_SYSTEM_CTROL0 && (value & OV5640_SOFT_RESET)) {
        ov5640_reset_regs(s);
        value &= ~OV5640_SOFT_RESET;
    }
    s->regs[reg] = value;
}

static int ov5640_event(I2CSlave *i2c, enum i2c_event event)
{
    OV5640State *s = OV5640(i2c);

    if (event == I2C_START_SEND) {
        s->addr_bytes = 2;
    }
    return 0;
}

static int ov5640_send(I2CSlave *i2c, uint8_t data)
{
    OV5640State *s = OV5640(i2c);

    if (s->addr_bytes) {
        s->addr = (s->addr << 8) | data;
        s->addr_bytes--;
    } else {
        ov5640_write_reg(s, s->addr++, data);
    }
    return 0;
}

static uint8_t ov5640_recv(I2CSlave *i2c)
{
    OV5640State *s = OV5640(i2c);

    return s->regs[s->addr++];
}

static uint16_t ov5640_reg16(OV5640State *s, uint16_t reg)
{
    return (s->regs[reg] << 8) | s->regs[reg + 1];
}

static GByteArray *ov5640_next_frame(Object *obj)
{
    OV5640State *s = OV5640(obj);

    if ((s->regs[OV5640_SYSTEM_CTROL0] & OV5640_POWER_DOWN) ||
        !(s->regs[OV5640_TIMING_TC_REG21] & OV5640_JPEG_EN)) {
        return NULL;
    }

    const unsigned width = ov5640_reg16(s, OV5640_X_OUTPUT_SIZE) & 0xfff;
    const unsigned height = ov5640_reg16(s, OV5640_Y_OUTPUT_SIZE) & 0x7ff;
    /* Quantization scale 0-63 to IJG quality 100-5 */
    const int qs = s->regs[OV5640_COMPRESSION_07] & 0x3f;
    const int quality = 100 - qs * 95 / 63;

    if (!width || !height) {
        return NULL;
    }
    if (!s->frame || width != s->frame_width || height != s->frame_height ||
        quality != s->frame_quality) {
        if (s->frame) {
            g_byte_array_unref(s->frame);
        }
        s->frame = cam_jpeg_test_pattern(width, height, quality,
                                         "QEMU OV5640");
        s->frame_width = width;
        s->frame_height = height;
        s->frame_quality = quality;
        trace_ov5640_frame(width, height, quality, s->frame->len);
    }
    return g_byte_array_ref(s->frame);
}

static void ov5640_reset_hold(Object *obj, ResetType type)
{
    OV5640State *s = OV5640(obj);

    ov5640_reset_regs(s);
    s->addr = 0;
    s->addr_bytes = 0;
}

static void ov5640_finalize(Object *obj)
{
    OV5640State *s = OV5640(obj);

    if (s->frame) {
        g_byte_array_unref(s->frame);
    }
}

static void ov5640_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DvpSensorClass *dc = DVP_SENSOR_CLASS(klass);

    k->event = ov5640_event;
    k->send = ov5640_send;
    k->recv = ov5640_recv;
    rc->phases.hold = ov5640_reset_hold;
    dc->next_frame = ov5640_next_frame;
}

static const TypeInfo ov5640_info = {
    .name = TYPE_OV5640,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(OV5640State),
    .instance_finalize = ov5640_finalize,
    .class_init = ov5640_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_DVP_SENSOR },
        { }
    },
};

static void ov5640_register_types(void)
{
    type_register_static(&ov5640_info);
}

type_init(ov5640_register_types)
