/*
 * ESP32-S3 I2C controller emulation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/sysbus.h"
#include "qemu/fifo8.h"
#include "hw/i2c/i2c.h"
#include "hw/registerfields.h"

#define TYPE_ESP32S3_I2C "esp32s3.i2c"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3I2CState, ESP32S3_I2C)

#define ESP32S3_I2C_MEM_SIZE    0x200
#define ESP32S3_I2C_FIFO_LENGTH 32
#define ESP32S3_I2C_CMD_COUNT   8

REG32(I2C_CTR, 0x04)
    FIELD(I2C_CTR, MS_MODE, 4, 1)
    FIELD(I2C_CTR, TRANS_START, 5, 1)
    FIELD(I2C_CTR, FSM_RST, 10, 1)
    FIELD(I2C_CTR, CONF_UPGATE, 11, 1)

REG32(I2C_SR, 0x08)
    FIELD(I2C_SR, BUS_BUSY, 4, 1)
    FIELD(I2C_SR, RXFIFO_CNT, 8, 6)
    FIELD(I2C_SR, TXFIFO_CNT, 18, 6)

REG32(I2C_FIFO_CONF, 0x18)
    FIELD(I2C_FIFO_CONF, NONFIFO_EN, 10, 1)
    FIELD(I2C_FIFO_CONF, RX_FIFO_RST, 12, 1)
    FIELD(I2C_FIFO_CONF, TX_FIFO_RST, 13, 1)

REG32(I2C_DATA, 0x1c)
REG32(I2C_INT_RAW, 0x20)
    FIELD(I2C_INT_RAW, END_DETECT, 3, 1)
    FIELD(I2C_INT_RAW, TRANS_COMPLETE, 7, 1)
    FIELD(I2C_INT_RAW, NACK, 10, 1)
REG32(I2C_INT_CLR, 0x24)
REG32(I2C_INT_ENA, 0x28)
REG32(I2C_INT_STATUS, 0x2c)

REG32(I2C_COMD0, 0x58)
    FIELD(I2C_COMD0, BYTE_NUM, 0, 8)
    FIELD(I2C_COMD0, ACK_EN, 8, 1)
    FIELD(I2C_COMD0, ACK_EXP, 9, 1)
    FIELD(I2C_COMD0, OP_CODE, 11, 3)
    FIELD(I2C_COMD0, DONE, 31, 1)

REG32(I2C_DATE, 0xf8)

/* Command opcodes, which differ from the ESP32's */
typedef enum {
    ESP32S3_I2C_OP_WRITE   = 1,
    ESP32S3_I2C_OP_STOP    = 2,
    ESP32S3_I2C_OP_READ    = 3,
    ESP32S3_I2C_OP_END     = 4,
    ESP32S3_I2C_OP_RSTART  = 6,
} ESP32S3I2COpcode;

struct ESP32S3I2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    /* Controller number, which names its bus "i2c<port>" */
    uint8_t port;
    I2CBus *bus;
    Fifo8 rx_fifo;
    Fifo8 tx_fifo;

    /* A START has been sent and the bus is held by this controller */
    bool bus_busy;
    /* The next byte written is a slave address (after a (re)START) */
    bool addr_next;

    uint32_t int_raw;
    uint32_t int_ena;
    /* Every register that only reads back what was written */
    uint32_t regs[ESP32S3_I2C_MEM_SIZE / 4];
};
