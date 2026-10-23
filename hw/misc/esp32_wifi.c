/*
 * ESP32 Wi-Fi MAC emulation - chip specific layer
 *
 * Register offsets come from MMIO traces of the IDF Wi-Fi driver, from the
 * esp32-open-mac project and from the earlier emulations by Martin Johnson,
 * Jasper Devreker and lcgamboa. All of them are reverse-engineered; their
 * confidence levels are listed in docs/wifi.md.
 *
 * Copyright (c) 2022 Martin Johnson
 * Copyright (c) 2023 Jasper Devreker (redfast00)
 * Copyright (c) 2023 lcgamboa
 * Copyright (c) 2026 Jason Kraus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/esp32_wifi.h"
#include "esp_wifi_int.h"

static const EspWifiMacLayout esp32_wifi_layout = {
    .size = 0x2000,
    .addr_filter = { 0x040, 0x048 },
    .rx_ctrl = 0x084,
    .rx_base = 0x088,
    .rx_next = 0x08c,
    .rx_last = 0x090,
    .int_status = 0xc48,
    .int_clear = 0xc4c,
    .mac_ctrl = 0xcb8,
    .mac_ctrl_busy = 0x2000,
    .txq_error_clear = 0xcbc,
    .txq_error = 0xcc0,
    .txq_complete_clear = 0xcc4,
    .txq_complete = 0xcc8,
    .tx_config0 = 0xd1c,
    .tx_plcp0 = 0xd20,
    .tx_stride = -8,
    .plcp1_0 = 0x1258,
    .plcp1_stride = -0x3c,
    .phy_tx_status = 0x1270,
    .int_rx = 0x1000024,
    .dma_base = 0x3ff00000,
    .status = {
        { 0x084, 0x1, 0x0 },            /* reload RX descriptors */
        { 0xd24, 0x2, 0x1 },            /* TX DMA stop request / idle */
    },
};

/* Entry 0 is the MAC itself; the others only absorb PHY calibration */
static const EspWifiRadioBlock esp32_wifi_radio[] = {
    { "mac",  0x3ff73000, 0x2000 },
    { "fe2",  0x3ff45000, 0x1000, true, 0xffffffff },
    { "fe",   0x3ff46000, 0x1000 },
    { "ana",  0x3ff4e000, 0x1000 },
    { "bt",   0x3ff51000, 0x1000 },
    { "nrx",  0x3ff5c000, 0x1000, true, 0xffffffff },
    { "bb",   0x3ff5d000, 0x1000, true, 0xffffffff },
    { "phy",  0x3ff71000, 0x1000, true, 0xffffffff },
    { "wdev", 0x3ff75000, 0x1000 },
};

static bool esp32_wifi_mac_reg_known(hwaddr addr)
{
    int slot;

    if (addr < 0x070) {                 /* BSSID and address filters */
        return true;
    }
    switch (addr) {
    case 0x084 ... 0x090:               /* RX descriptors */
    case 0xd24:
    case 0x0d8 ... 0x0e4:               /* RX policy */
    case 0xc48:
    case 0xc4c:
    case 0xcb8 ... 0xcc8:
    case 0x1270:
        return true;
    }
    for (slot = 0; slot < ESP_WIFI_TX_SLOTS; slot++) {
        hwaddr plcp = 0x1258 - 0x3c * slot;

        if (addr == 0xd1c - 8 * slot || (addr >= plcp && addr <= plcp + 0x10)) {
            return true;
        }
    }
    return false;
}

static void esp32_wifi_fill_rx_ctrl(EspWifiState *s, uint8_t *hdr,
                                    const EspWifiFrame *f)
{
    stl_le_p(hdr, esp_wifi_rx_ctrl_word0(s, f));
    stl_le_p(hdr + 8, deposit32((uint8_t)-97, 16, 4, s->channel));
    stl_le_p(hdr + 12, esp_wifi_tsf(s));
    stl_le_p(hdr + 24, deposit32(f->len, 12, 12, f->len));
}

static uint64_t esp32_wifi_radio_read(EspWifiState *s,
                                      const EspWifiRadioBlock *b,
                                      hwaddr addr, uint64_t value)
{
    /* Calibration and PLL lock status bits. reverse-engineered */
    if (!strcmp(b->name, "ana")) {
        switch (addr) {
        case 0x04:
            return 0xfdffffff;
        case 0x44:
        case 0x4c:
        case 0xc4:
            return 0xffffffff;
        }
    } else if (!strcmp(b->name, "fe")) {
        if (addr == 0x7c || addr == 0x174) {
            return 0xffffffff;
        }
    }
    return value;
}

static void esp32_wifi_radio_write(EspWifiState *s, const EspWifiRadioBlock *b,
                                   hwaddr addr, uint64_t value)
{
    /*
     * The driver programs the RF PLL through this register when it changes
     * channel; the low byte reads 10 * (channel + 1) + 4 at the end of the
     * sequence. reverse-engineered
     */
    if (!strcmp(b->name, "ana") && addr == 0xc4) {
        uint32_t v = value & 0xff;

        if (v % 10 == 4 && v / 10 >= 2 && v / 10 <= 15) {
            esp_wifi_set_channel(s, v / 10 - 1);
        }
    }
}

static void esp32_wifi_class_init(ObjectClass *klass, void *data)
{
    EspWifiClass *k = ESP_WIFI_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "ESP32 Wi-Fi MAC";
    k->layout = &esp32_wifi_layout;
    k->radio = esp32_wifi_radio;
    k->n_radio = ARRAY_SIZE(esp32_wifi_radio);
    k->rx_ctrl_size = 28;
    k->fill_rx_ctrl = esp32_wifi_fill_rx_ctrl;
    k->mac_reg_known = esp32_wifi_mac_reg_known;
    k->radio_read = esp32_wifi_radio_read;
    k->radio_write = esp32_wifi_radio_write;
}

static const TypeInfo esp32_wifi_info = {
    .name = TYPE_ESP32_WIFI,
    .parent = TYPE_ESP_WIFI,
    .class_init = esp32_wifi_class_init,
};

static void esp32_wifi_register_types(void)
{
    type_register_static(&esp32_wifi_info);
}

type_init(esp32_wifi_register_types)
