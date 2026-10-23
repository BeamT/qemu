/*
 * ESP32-C3 Wi-Fi MAC emulation - chip specific layer
 *
 * The ESP32-C3 MAC is a relative of the ESP32 one: the RX side and the
 * address filters sit at the same offsets, the interrupt and TX registers
 * moved. Offsets come from the ESP32-C3 port by lcgamboa and from MMIO traces
 * of the IDF Wi-Fi driver; see docs/wifi.md for confidence levels.
 *
 * Copyright (c) 2023 lcgamboa
 * Copyright (c) 2026 Jason Kraus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/esp32c3_wifi.h"
#include "esp_wifi_int.h"

static const EspWifiMacLayout esp32c3_wifi_layout = {
    .size = 0x2000,
    .addr_filter = { 0x040, 0x048 },
    .rx_ctrl = 0x084,
    .rx_base = 0x088,
    .rx_next = 0x08c,
    .rx_last = 0x090,
    .int_status = 0xc3c,
    .int_clear = 0xc40,
    .txq_error_clear = 0xca4,
    .txq_error = 0xca8,
    .txq_complete_clear = 0xcac,
    .txq_complete = 0xcb0,
    .tx_config0 = 0xd04,
    .tx_plcp0 = 0xd08,
    .tx_stride = -8,
    .phy_tx_status = 0x1320,
    .int_rx = 0x1004000,
    .dma_base = 0x3fc00000,
    .status = {
        { 0x084, 0x1, 0x0 },            /* reload RX descriptors */
        { 0xd14, 0x2, 0x1 },            /* TX DMA stop request / idle */
    },
};

static const EspWifiRadioBlock esp32c3_wifi_radio[] = {
    { "mac",  0x60033000, 0x2000 },
    { "fe2",  0x60005000, 0x1000 },
    { "fe",   0x60006000, 0x1000 },
    { "ana",  0x6000e000, 0x1000 },
    { "nrx",  0x6001c000, 0x1000, true, 0xffffffff },
    { "bb",   0x6001d000, 0x1000, true, 0xffffffff },
    { "pwr",  0x60035000, 0x1000 },
};

static bool esp32c3_wifi_mac_reg_known(hwaddr addr)
{
    if (addr < 0x070) {
        return true;
    }
    switch (addr) {
    case 0x084 ... 0x090:
    case 0xc3c:
    case 0xc40:
    case 0xca4 ... 0xcb0:
    case 0xd14:
    case 0x1320:
        return true;
    }
    return false;
}

static void esp32c3_wifi_fill_rx_ctrl(EspWifiState *s, uint8_t *hdr,
                                      const EspWifiFrame *f)
{
    stl_le_p(hdr, esp_wifi_rx_ctrl_word0(s, f));
    stl_le_p(hdr + 8, deposit32(0, 16, 4, s->channel));
    stl_le_p(hdr + 12, esp_wifi_tsf(s));
    stl_le_p(hdr + 20, (uint8_t)-97);
    stl_le_p(hdr + 44, deposit32(f->len, 12, 12, f->len));
}

static uint64_t esp32c3_wifi_radio_read(EspWifiState *s,
                                        const EspWifiRadioBlock *b,
                                        hwaddr addr, uint64_t value)
{
    /*
     * The analog block is reached through the RTC I2C master: status words
     * read as "done", PLL lock bits as set. reverse-engineered
     */
    if (!strcmp(b->name, "ana")) {
        switch (addr) {
        case 0x00:
        case 0x48:
            return 0x00ffffff;
        case 0x04:
            return 0xfdffffff;
        case 0x50:
            return value | 0x07000000;
        case 0x44:
        case 0x4c:
        case 0xc4:
            return 0xffffffff;
        }
    } else if (!strcmp(b->name, "fe")) {
        if (addr == 0x7c || addr == 0x174) {
            return 0xffffffff;
        }
    } else if (!strcmp(b->name, "pwr")) {
        switch (addr) {
        case 0x118:                     /* pending power events */
        case 0x128:
            return 0;
        }
    }
    return value;
}

static void esp32c3_wifi_radio_write(EspWifiState *s,
                                     const EspWifiRadioBlock *b,
                                     hwaddr addr, uint64_t value)
{
    /*
     * RF PLL frequency, in MHz above 2400, written on every channel change:
     * channel n is at 2407 + 5 * n MHz. reverse-engineered
     */
    if (!strcmp(b->name, "ana") && addr == 0x150) {
        uint32_t mhz = extract32(value, 20, 8);

        if (mhz >= 12 && (mhz - 7) % 5 == 0 && (mhz - 7) / 5 <= 13) {
            esp_wifi_set_channel(s, (mhz - 7) / 5);
        } else if (mhz == 84) {
            esp_wifi_set_channel(s, 14);
        }
    }
}

static void esp32c3_wifi_class_init(ObjectClass *klass, void *data)
{
    EspWifiClass *k = ESP_WIFI_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "ESP32-C3 Wi-Fi MAC";
    k->layout = &esp32c3_wifi_layout;
    k->radio = esp32c3_wifi_radio;
    k->n_radio = ARRAY_SIZE(esp32c3_wifi_radio);
    k->rx_ctrl_size = 48;
    k->fill_rx_ctrl = esp32c3_wifi_fill_rx_ctrl;
    k->mac_reg_known = esp32c3_wifi_mac_reg_known;
    k->radio_read = esp32c3_wifi_radio_read;
    k->radio_write = esp32c3_wifi_radio_write;
}

static const TypeInfo esp32c3_wifi_info = {
    .name = TYPE_ESP32C3_WIFI,
    .parent = TYPE_ESP_WIFI,
    .class_init = esp32c3_wifi_class_init,
};

static void esp32c3_wifi_register_types(void)
{
    type_register_static(&esp32c3_wifi_info);
}

type_init(esp32c3_wifi_register_types)
