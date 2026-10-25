/*
 * ESP32-S3 I2C controller emulation
 *
 * Master mode only. A transaction is the list of up to 8 commands in
 * COMD0..7, run when CTR.TRANS_START is set: (re)START, WRITE (from the TX
 * FIFO), READ (into the RX FIFO), STOP or END. END pauses the transaction
 * with the bus still held, so the driver can refill the FIFO and commands
 * and start again from COMD0, which is how ESP-IDF's i2c_master driver
 * sends anything longer than the FIFO. The whole list runs at once, at
 * TRANS_START; there is no bus timing.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/i2c/esp32s3_i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"

#define I2C_INT_RXFIFO_OVF      BIT(2)
#define I2C_INT_MST_TXFIFO_UDF  BIT(6)

static void esp32s3_i2c_update_irq(ESP32S3I2CState *s)
{
    qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
}

static void esp32s3_i2c_release_bus(ESP32S3I2CState *s)
{
    if (s->bus_busy) {
        i2c_end_transfer(s->bus);
    }
    s->bus_busy = false;
    s->addr_next = false;
}

/*
 * Run one WRITE command. Returns false if the transaction must stop
 * because a byte was not acknowledged as expected.
 */
static bool esp32s3_i2c_write_bytes(ESP32S3I2CState *s, uint32_t cmd)
{
    const int count = FIELD_EX32(cmd, I2C_COMD0, BYTE_NUM);

    for (int i = 0; i < count; i++) {
        if (fifo8_is_empty(&s->tx_fifo)) {
            s->int_raw |= I2C_INT_MST_TXFIFO_UDF;
            return true;
        }
        const uint8_t byte = fifo8_pop(&s->tx_fifo);
        int nack;
        if (s->addr_next) {
            nack = i2c_start_transfer(s->bus, byte >> 1, byte & 1);
            s->addr_next = false;
        } else {
            nack = i2c_send(s->bus, byte);
        }
        if (nack && FIELD_EX32(cmd, I2C_COMD0, ACK_EN) &&
            !FIELD_EX32(cmd, I2C_COMD0, ACK_EXP)) {
            s->int_raw |= R_I2C_INT_RAW_NACK_MASK;
            esp32s3_i2c_release_bus(s);
            return false;
        }
    }
    return true;
}

static void esp32s3_i2c_read_bytes(ESP32S3I2CState *s, uint32_t cmd)
{
    const int count = FIELD_EX32(cmd, I2C_COMD0, BYTE_NUM);

    for (int i = 0; i < count; i++) {
        const uint8_t byte = i2c_recv(s->bus);
        if (fifo8_is_full(&s->rx_fifo)) {
            s->int_raw |= I2C_INT_RXFIFO_OVF;
        } else {
            fifo8_push(&s->rx_fifo, byte);
        }
    }
}

static void esp32s3_i2c_run(ESP32S3I2CState *s)
{
    for (int i = 0; i < ESP32S3_I2C_CMD_COUNT; i++) {
        uint32_t *cmd = &s->regs[(A_I2C_COMD0 / 4) + i];
        const int op = FIELD_EX32(*cmd, I2C_COMD0, OP_CODE);
        bool next = true;

        switch (op) {
        case ESP32S3_I2C_OP_RSTART:
            s->bus_busy = true;
            s->addr_next = true;
            break;
        case ESP32S3_I2C_OP_WRITE:
            next = esp32s3_i2c_write_bytes(s, *cmd);
            break;
        case ESP32S3_I2C_OP_READ:
            esp32s3_i2c_read_bytes(s, *cmd);
            break;
        case ESP32S3_I2C_OP_STOP:
            esp32s3_i2c_release_bus(s);
            s->int_raw |= R_I2C_INT_RAW_TRANS_COMPLETE_MASK;
            next = false;
            break;
        case ESP32S3_I2C_OP_END:
            s->int_raw |= R_I2C_INT_RAW_END_DETECT_MASK;
            next = false;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32s3_i2c: invalid opcode %d in COMD%d\n", op, i);
            next = false;
            break;
        }
        *cmd = FIELD_DP32(*cmd, I2C_COMD0, DONE, 1);
        if (!next) {
            break;
        }
    }
    esp32s3_i2c_update_irq(s);
}

static uint64_t esp32s3_i2c_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3I2CState *s = ESP32S3_I2C(opaque);
    uint32_t r;

    switch (addr) {
    case A_I2C_SR:
        r = FIELD_DP32(0, I2C_SR, BUS_BUSY, s->bus_busy);
        r = FIELD_DP32(r, I2C_SR, RXFIFO_CNT, fifo8_num_used(&s->rx_fifo));
        return FIELD_DP32(r, I2C_SR, TXFIFO_CNT, fifo8_num_used(&s->tx_fifo));
    case A_I2C_DATA:
        if (fifo8_is_empty(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "esp32s3_i2c: RX FIFO empty\n");
            return 0;
        }
        return fifo8_pop(&s->rx_fifo);
    case A_I2C_INT_RAW:
        return s->int_raw;
    case A_I2C_INT_ENA:
        return s->int_ena;
    case A_I2C_INT_STATUS:
        return s->int_raw & s->int_ena;
    case A_I2C_INT_CLR:
        return 0;
    default:
        return s->regs[addr / 4];
    }
}

static void esp32s3_i2c_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    ESP32S3I2CState *s = ESP32S3_I2C(opaque);

    switch (addr) {
    case A_I2C_CTR:
        if (FIELD_EX32(value, I2C_CTR, FSM_RST)) {
            esp32s3_i2c_release_bus(s);
        }
        /* TRANS_START, FSM_RST and CONF_UPGATE clear themselves */
        s->regs[addr / 4] = value & ~(R_I2C_CTR_TRANS_START_MASK |
                                      R_I2C_CTR_FSM_RST_MASK |
                                      R_I2C_CTR_CONF_UPGATE_MASK);
        if (FIELD_EX32(value, I2C_CTR, TRANS_START)) {
            if (!FIELD_EX32(value, I2C_CTR, MS_MODE)) {
                qemu_log_mask(LOG_UNIMP, "esp32s3_i2c: slave mode\n");
            } else {
                esp32s3_i2c_run(s);
            }
        }
        break;
    case A_I2C_SR:
    case A_I2C_INT_STATUS:
    case A_I2C_DATE:
        break;
    case A_I2C_FIFO_CONF:
        if (FIELD_EX32(value, I2C_FIFO_CONF, NONFIFO_EN)) {
            qemu_log_mask(LOG_UNIMP, "esp32s3_i2c: non-FIFO mode\n");
        }
        if (FIELD_EX32(value, I2C_FIFO_CONF, RX_FIFO_RST)) {
            fifo8_reset(&s->rx_fifo);
        }
        if (FIELD_EX32(value, I2C_FIFO_CONF, TX_FIFO_RST)) {
            fifo8_reset(&s->tx_fifo);
        }
        s->regs[addr / 4] = value;
        break;
    case A_I2C_DATA:
        if (fifo8_is_full(&s->tx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "esp32s3_i2c: TX FIFO full\n");
        } else {
            fifo8_push(&s->tx_fifo, value);
        }
        break;
    case A_I2C_INT_CLR:
        s->int_raw &= ~value;
        esp32s3_i2c_update_irq(s);
        break;
    case A_I2C_INT_ENA:
        s->int_ena = value;
        esp32s3_i2c_update_irq(s);
        break;
    case A_I2C_INT_RAW:
        break;
    default:
        s->regs[addr / 4] = value;
        break;
    }
}

static const MemoryRegionOps esp32s3_i2c_ops = {
    .read = esp32s3_i2c_read,
    .write = esp32s3_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void esp32s3_i2c_reset_hold(Object *obj, ResetType type)
{
    ESP32S3I2CState *s = ESP32S3_I2C(obj);

    esp32s3_i2c_release_bus(s);
    fifo8_reset(&s->rx_fifo);
    fifo8_reset(&s->tx_fifo);
    s->int_raw = 0;
    s->int_ena = 0;
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[A_I2C_DATE / 4] = 0x20070201;
    qemu_irq_lower(s->irq);
}

static void esp32s3_i2c_init(Object *obj)
{
    ESP32S3I2CState *s = ESP32S3_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_i2c_ops, s,
                          TYPE_ESP32S3_I2C, ESP32S3_I2C_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    fifo8_create(&s->tx_fifo, ESP32S3_I2C_FIFO_LENGTH);
    fifo8_create(&s->rx_fifo, ESP32S3_I2C_FIFO_LENGTH);
}

static void esp32s3_i2c_realize(DeviceState *dev, Error **errp)
{
    ESP32S3I2CState *s = ESP32S3_I2C(dev);
    g_autofree char *name = g_strdup_printf("i2c%u", s->port);

    s->bus = i2c_init_bus(dev, name);
}

static Property esp32s3_i2c_properties[] = {
    DEFINE_PROP_UINT8("port", ESP32S3I2CState, port, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = esp32s3_i2c_realize;
    rc->phases.hold = esp32s3_i2c_reset_hold;
    device_class_set_props(dc, esp32s3_i2c_properties);
}

static const TypeInfo esp32s3_i2c_type_info = {
    .name = TYPE_ESP32S3_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3I2CState),
    .instance_init = esp32s3_i2c_init,
    .class_init = esp32s3_i2c_class_init,
};

static void esp32s3_i2c_register_types(void)
{
    type_register_static(&esp32s3_i2c_type_info);
}

type_init(esp32s3_i2c_register_types)
