/*
 * Espressif Wi-Fi emulation - interface between the MAC core and the
 * emulated wireless medium (access points, SoftAP clients, ESP-NOW peers).
 *
 * Copyright (c) 2026 Jason Kraus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "hw/misc/esp_wifi.h"

/* IEEE 802.11 frame layout */
#define WLAN_HDR_LEN            24
#define WLAN_FCS_LEN            4
#define WLAN_FC_TYPE(fc)        (((fc) >> 2) & 3)
#define WLAN_FC_SUBTYPE(fc)     (((fc) >> 4) & 0xf)
#define WLAN_FC_TODS            0x0100
#define WLAN_FC_FROMDS          0x0200
#define WLAN_FC_PROTECTED       0x4000

#define WLAN_TYPE_MGMT          0
#define WLAN_TYPE_CTRL          1
#define WLAN_TYPE_DATA          2

#define WLAN_MGMT_ASSOC_REQ     0
#define WLAN_MGMT_ASSOC_RESP    1
#define WLAN_MGMT_REASSOC_REQ   2
#define WLAN_MGMT_REASSOC_RESP  3
#define WLAN_MGMT_PROBE_REQ     4
#define WLAN_MGMT_PROBE_RESP    5
#define WLAN_MGMT_BEACON        8
#define WLAN_MGMT_DISASSOC      10
#define WLAN_MGMT_AUTH          11
#define WLAN_MGMT_DEAUTH        12
#define WLAN_MGMT_ACTION        13
#define WLAN_CTRL_ACK           13
#define WLAN_DATA_QOS           0x8     /* subtype bit */
#define WLAN_DATA_NULL          0x4     /* subtype bit */

#define WLAN_EID_SSID           0
#define WLAN_EID_RATES          1
#define WLAN_EID_DS_PARAMS      3
#define WLAN_EID_TIM            5
#define WLAN_EID_RSN            48
#define WLAN_EID_EXT_RATES      50

/*
 * Frames on the emulated medium that are not translated to Ethernet (ESP-NOW
 * and other action frames, and their ACKs) travel between QEMU instances
 * inside Ethernet frames with this local experimental EtherType.
 */
#define ESP_WIFI_AIR_ETHERTYPE  0x88b5
#define ESP_WIFI_AIR_MAGIC      "QWIF"

void esp_wifi_air_init(EspWifiState *s);
void esp_wifi_air_reset(EspWifiState *s);
void esp_wifi_air_realize(EspWifiState *s, Error **errp);
/* Returns true if the transmission must wait for an ACK from a peer */
bool esp_wifi_air_tx(EspWifiState *s, int slot, const uint8_t *frame,
                     size_t len);
void esp_wifi_air_rx_eth(EspWifiState *s, const uint8_t *buf, size_t len);
void esp_wifi_air_channel_changed(EspWifiState *s);
void esp_wifi_air_ap_changed(EspWifiState *s, int i, Error **errp);

static inline uint16_t wlan_fc(const uint8_t *f)
{
    return f[0] | (f[1] << 8);
}

static inline bool wlan_is_group(const uint8_t *addr)
{
    return addr[0] & 1;
}

/*
 * First word of the RX control header the MAC writes in front of a frame,
 * common to all chips: RSSI, rate, length and address filter matches.
 * reverse-engineered
 */
static inline uint32_t esp_wifi_rx_ctrl_word0(EspWifiState *s,
                                              const EspWifiFrame *f)
{
    const uint8_t *a1 = f->data + 4, *a3 = f->data + 16;
    const uint8_t *m0 = esp_wifi_own_mac(s, 0);
    const uint8_t *m1 = esp_wifi_own_mac(s, 1);
    uint32_t w = 0;

    w = deposit32(w, 0, 8, (uint8_t)(f->rssi +
                                     ESP_WIFI_GET_CLASS(s)->rssi_offset));
    w = deposit32(w, 8, 5, 11);
    w = deposit32(w, 16, 12, f->len);
    w = deposit32(w, 28, 1, wlan_is_group(a1) || !memcmp(a1, m0, 6));
    w = deposit32(w, 29, 1, wlan_is_group(a1) || !memcmp(a1, m1, 6));
    w = deposit32(w, 30, 1, wlan_is_group(a3) || !memcmp(a3, m0, 6));
    w = deposit32(w, 31, 1, wlan_is_group(a3) || !memcmp(a3, m1, 6));
    return w;
}
