/*
 * Espressif Wi-Fi emulation - the wireless medium
 *
 * Everything the guest transmits ends up here, and everything it receives
 * comes from here. Three kinds of peers are emulated:
 *
 *  - access points, configured with properties, that the guest station can
 *    scan for and join. Their traffic is bridged to the netdev as Ethernet.
 *  - stations that join the guest's own SoftAP, one per host on the netdev
 *    that sends traffic to it.
 *  - other QEMU instances on the same netdev, for frames that cannot be
 *    translated to Ethernet (ESP-NOW and other action frames). These travel
 *    whole, tunnelled in Ethernet frames, and are acknowledged like on air.
 *
 * WPA2-PSK networks run a real 4-way handshake so the driver installs its
 * keys. Data frames themselves are exchanged in clear, as the MAC hardware
 * would have encrypted and decrypted them.
 *
 * The AP emulation started from the QEMU WLAN access point emulation by
 * Clemens Kolbitsch (2008, MIT licence), as adapted for the ESP32 by Martin
 * Johnson and extended for SoftAP and ESP-NOW by lcgamboa. It has been
 * rewritten; see docs/wifi-provenance.md.
 *
 * Copyright (c) 2008 Clemens Kolbitsch
 * Copyright (c) 2022 Martin Johnson
 * Copyright (c) 2023 lcgamboa
 * Copyright (c) 2026 Jason Kraus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/guest-random.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "crypto/hmac.h"
#include "crypto/pbkdf.h"
#include "crypto/cipher.h"
#include "hw/misc/esp_wifi.h"
#include "esp_wifi_int.h"
#include "trace.h"

#define TU_NS                   (1024 * SCALE_US)
#define CLIENT_RETRY_NS         (500 * SCALE_MS)
#define CLIENT_TICK_NS          (100 * SCALE_MS)
#define CLIENT_PENDING_MAX      32
#define EAPOL_FIRST_NS          (10 * SCALE_MS)
#define EAPOL_RETRY_NS          (1000 * SCALE_MS)
#define EAPOL_TRIES             4

#define ETH_HLEN                14
#define ETH_P_EAPOL             0x888e
#define LLC_SNAP_LEN            8

#define AIR_HDR_LEN             8
#define AIR_RSSI                (-40)

/* EAPOL-Key information bits */
#define KEY_INFO_PAIRWISE       0x0008
#define KEY_INFO_INSTALL        0x0040
#define KEY_INFO_ACK            0x0080
#define KEY_INFO_MIC            0x0100
#define KEY_INFO_SECURE         0x0200
#define KEY_INFO_ENCRYPTED      0x1000

/* Offsets in an EAPOL-Key frame, counted from the EAPOL header */
#define EAPOL_HDR_LEN           4
#define EK_INFO                 (EAPOL_HDR_LEN + 1)
#define EK_REPLAY               (EAPOL_HDR_LEN + 5)
#define EK_NONCE                (EAPOL_HDR_LEN + 13)
#define EK_MIC                  (EAPOL_HDR_LEN + 77)
#define EK_DATA_LEN             (EAPOL_HDR_LEN + 93)
#define EK_DATA                 (EAPOL_HDR_LEN + 95)

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t llc_snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

/* RSN IE: CCMP group and pairwise cipher, PSK key management */
static const uint8_t rsn_ie[] = {
    WLAN_EID_RSN, 20, 0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,
    0x00, 0x00,
};

/* Frame builder */

typedef struct WBuf {
    uint8_t d[ESP_WIFI_FRAME_MAX];
    size_t len;
} WBuf;

static void wb_put(WBuf *b, const void *data, size_t n)
{
    assert(b->len + n <= sizeof(b->d));
    memcpy(b->d + b->len, data, n);
    b->len += n;
}

static void wb_u8(WBuf *b, uint8_t v)
{
    wb_put(b, &v, 1);
}

static void wb_le16(WBuf *b, uint16_t v)
{
    uint8_t t[2];

    stw_le_p(t, v);
    wb_put(b, t, 2);
}

static void wb_ie(WBuf *b, uint8_t id, const void *data, uint8_t n)
{
    wb_u8(b, id);
    wb_u8(b, n);
    wb_put(b, data, n);
}

static void wb_hdr(EspWifiState *s, WBuf *b, unsigned type, unsigned subtype,
                   uint16_t flags, const uint8_t *a1, const uint8_t *a2,
                   const uint8_t *a3)
{
    b->len = 0;
    wb_le16(b, (type << 2) | (subtype << 4) | flags);
    wb_le16(b, 0);
    wb_put(b, a1, 6);
    wb_put(b, a2, 6);
    wb_put(b, a3, 6);
    wb_le16(b, (s->seq++ & 0xfff) << 4);
}

static const uint8_t *wlan_find_ie(const uint8_t *ies, size_t len, uint8_t id)
{
    size_t i = 0;

    while (i + 2 <= len && i + 2 + ies[i + 1] <= len) {
        if (ies[i] == id) {
            return &ies[i];
        }
        i += 2 + ies[i + 1];
    }
    return NULL;
}

static void wb_rates(WBuf *b)
{
    static const uint8_t rates[] = {
        0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24
    };
    static const uint8_t ext_rates[] = { 0x30, 0x48, 0x60, 0x6c };

    wb_ie(b, WLAN_EID_RATES, rates, sizeof(rates));
    wb_ie(b, WLAN_EID_EXT_RATES, ext_rates, sizeof(ext_rates));
}

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bool is_own_mac(EspWifiState *s, const uint8_t *addr)
{
    return mac_eq(addr, esp_wifi_own_mac(s, 0)) ||
           mac_eq(addr, esp_wifi_own_mac(s, 1));
}

/* Crypto for the WPA2-PSK 4-way handshake (IEEE 802.11-2020 12.7) */

static bool wpa_pmk(const char *psk, const char *ssid, uint8_t *pmk)
{
    Error *err = NULL;

    if (qcrypto_pbkdf2(QCRYPTO_HASH_ALGO_SHA1, (const uint8_t *)psk,
                       strlen(psk), (const uint8_t *)ssid, strlen(ssid),
                       4096, pmk, 32, &err) < 0) {
        error_report_err(err);
        return false;
    }
    return true;
}

static void hmac(QCryptoHashAlgo alg, const uint8_t *key, size_t nkey,
                 const uint8_t *data, size_t len, uint8_t *out)
{
    g_autoptr(QCryptoHmac) h = NULL;
    g_autofree uint8_t *res = NULL;
    size_t nres = 0;

    h = qcrypto_hmac_new(alg, key, nkey, &error_abort);
    qcrypto_hmac_bytes(h, (const char *)data, len, &res, &nres, &error_abort);
    memcpy(out, res, nres);
}

static void aes_ecb(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    g_autoptr(QCryptoCipher) c = NULL;

    c = qcrypto_cipher_new(QCRYPTO_CIPHER_ALGO_AES_128,
                           QCRYPTO_CIPHER_MODE_ECB, key, 16, &error_abort);
    qcrypto_cipher_encrypt(c, in, out, 16, &error_abort);
}

static void cmac_subkey(uint8_t *k)
{
    uint8_t msb = k[0] & 0x80;
    int i;

    for (i = 0; i < 15; i++) {
        k[i] = (k[i] << 1) | (k[i + 1] >> 7);
    }
    k[15] = (k[15] << 1) ^ (msb ? 0x87 : 0);
}

/* AES-128-CMAC (RFC 4493) */
static void aes_cmac(const uint8_t *key, const uint8_t *msg, size_t len,
                     uint8_t *mac)
{
    uint8_t k[16] = {}, x[16] = {}, last[16] = {};
    size_t n = len ? (len + 15) / 16 : 1;
    size_t i, j;

    aes_ecb(key, k, k);
    cmac_subkey(k);                     /* K1 */
    if (len == 0 || len % 16) {
        size_t rem = len - (n - 1) * 16;

        memcpy(last, msg + (n - 1) * 16, rem);
        last[rem] = 0x80;
        cmac_subkey(k);                 /* K2 */
    } else {
        memcpy(last, msg + (n - 1) * 16, 16);
    }
    for (i = 0; i < n; i++) {
        const uint8_t *m = i == n - 1 ? last : msg + i * 16;

        for (j = 0; j < 16; j++) {
            x[j] ^= m[j] ^ (i == n - 1 ? k[j] : 0);
        }
        aes_ecb(key, x, x);
    }
    memcpy(mac, x, 16);
}

/*
 * Derive the PTK: PRF-384 with HMAC-SHA1 for AKM 00-0f-ac:2 (PSK), KDF-384
 * with HMAC-SHA256 for AKM 00-0f-ac:6 (PSK-SHA256, used with PMF).
 */
static void wpa_ptk(EspWifiRsnKeys *k, const uint8_t *pmk,
                    const uint8_t *aa, const uint8_t *spa)
{
    static const char label[] = "Pairwise key expansion";
    uint8_t data[2 + sizeof(label) + 6 + 6 + 32 + 32 + 2];
    uint8_t out[64];
    bool a_first = memcmp(aa, spa, 6) < 0;
    bool an_first = memcmp(k->anonce, k->snonce, 32) < 0;
    uint8_t *ctx = data + 2 + sizeof(label) - 1;
    uint8_t *p = ctx;
    int i;

    memcpy(data + 2, label, sizeof(label) - 1);
    memcpy(p, a_first ? aa : spa, 6);
    memcpy(p + 6, a_first ? spa : aa, 6);
    p += 12;
    memcpy(p, an_first ? k->anonce : k->snonce, 32);
    memcpy(p + 32, an_first ? k->snonce : k->anonce, 32);
    p += 64;

    if (k->akm == 6) {
        /* i || label || context || length, all counters little endian */
        stw_le_p(p, 384);
        for (i = 0; i < 2; i++) {
            stw_le_p(data, i + 1);
            hmac(QCRYPTO_HASH_ALGO_SHA256, pmk, 32, data, p + 2 - data,
                 out + i * 32);
        }
    } else {
        /* label || 0 || context || i */
        uint8_t *prf = data + 2;

        memmove(prf + sizeof(label), ctx, 76);
        prf[sizeof(label) - 1] = 0;
        for (i = 0; i < 3; i++) {
            prf[sizeof(label) + 76] = i;
            hmac(QCRYPTO_HASH_ALGO_SHA1, pmk, 32, prf, sizeof(label) + 77,
                 out + i * 20);
        }
    }
    memcpy(k->ptk, out, sizeof(k->ptk));
    k->ptk_valid = true;
}

/* Key descriptor version: 2 is HMAC-SHA1 and AES key wrap, 3 is AES-CMAC */
static uint16_t key_version(const EspWifiRsnKeys *k)
{
    return k->akm == 6 ? 3 : 2;
}

static void eapol_mic(const EspWifiRsnKeys *k, uint8_t *eapol, size_t len)
{
    uint8_t mic[32];

    memset(eapol + EK_MIC, 0, 16);
    if (k->akm == 6) {
        aes_cmac(k->ptk, eapol, len, mic);
    } else {
        hmac(QCRYPTO_HASH_ALGO_SHA1, k->ptk, 16, eapol, len, mic);
    }
    memcpy(eapol + EK_MIC, mic, 16);
}

static bool eapol_mic_ok(const EspWifiRsnKeys *k, const uint8_t *eapol,
                         size_t len)
{
    g_autofree uint8_t *copy = g_memdup2(eapol, len);
    uint8_t mic[16];

    memcpy(mic, eapol + EK_MIC, 16);
    eapol_mic(k, copy, len);
    return memcmp(mic, copy + EK_MIC, 16) == 0;
}

/* AES key wrap (RFC 3394) of the key data, with the KEK */
static size_t aes_wrap(const uint8_t *kek, const uint8_t *in, size_t n,
                       uint8_t *out)
{
    size_t blocks = n / 8;
    uint8_t a[8], b[16];
    int i, j;

    memset(a, 0xa6, 8);
    memcpy(out + 8, in, n);
    for (j = 0; j < 6; j++) {
        for (i = 1; i <= blocks; i++) {
            uint64_t t = blocks * j + i;

            memcpy(b, a, 8);
            memcpy(b + 8, out + i * 8, 8);
            aes_ecb(kek, b, b);
            memcpy(a, b, 8);
            a[7] ^= t & 0xff;
            a[6] ^= (t >> 8) & 0xff;
            memcpy(out + i * 8, b + 8, 8);
        }
    }
    memcpy(out, a, 8);
    return n + 8;
}

/*
 * Build an EAPOL-Key frame (LLC/SNAP included) into @b, after the 802.11
 * header already there.
 */
static uint8_t *wb_eapol_key(WBuf *b, uint16_t info, uint64_t replay,
                             const uint8_t *nonce, const uint8_t *data,
                             size_t ndata, size_t *eapol_len)
{
    uint8_t *eapol;
    uint8_t zero[32] = {};
    uint8_t t[8];
    size_t body = 95 + ndata;

    wb_put(b, llc_snap, 6);
    wb_u8(b, ETH_P_EAPOL >> 8);
    wb_u8(b, ETH_P_EAPOL & 0xff);
    eapol = b->d + b->len;
    wb_u8(b, 2);                        /* 802.1X-2004 */
    wb_u8(b, 3);                        /* EAPOL-Key */
    wb_u8(b, body >> 8);
    wb_u8(b, body & 0xff);
    wb_u8(b, 2);                        /* RSN key descriptor */
    wb_u8(b, info >> 8);
    wb_u8(b, info & 0xff);
    wb_u8(b, 0);
    wb_u8(b, (info & KEY_INFO_ACK) ? 16 : 0);
    stq_be_p(t, replay);
    wb_put(b, t, 8);
    wb_put(b, nonce ? nonce : zero, 32);
    wb_put(b, zero, 16);                /* IV */
    wb_put(b, zero, 8);                 /* RSC */
    wb_put(b, zero, 8);                 /* reserved */
    wb_put(b, zero, 16);                /* MIC */
    wb_u8(b, ndata >> 8);
    wb_u8(b, ndata & 0xff);
    wb_put(b, data, ndata);
    *eapol_len = EAPOL_HDR_LEN + body;
    return eapol;
}

/* Emulated access points */

static bool ap_configured(const EspWifiAp *ap)
{
    return ap->ssid && *ap->ssid;
}

static bool ap_rsn(const EspWifiAp *ap)
{
    return ap->psk && *ap->psk;
}

static bool ap_on_air(EspWifiState *s, const EspWifiAp *ap)
{
    /* Before the channel is known, every AP can be heard */
    return ap_configured(ap) && (!s->channel || ap->channel == s->channel);
}

static EspWifiAp *ap_by_bssid(EspWifiState *s, const uint8_t *bssid)
{
    int i;

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        if (ap_configured(&s->ap[i]) && mac_eq(s->ap[i].bssid.a, bssid)) {
            return &s->ap[i];
        }
    }
    return NULL;
}

static void ap_send_info(EspWifiState *s, EspWifiAp *ap, const uint8_t *dst,
                         unsigned subtype)
{
    WBuf b;
    uint8_t ts[8];
    uint8_t ch = ap->channel;
    static const uint8_t tim[] = { 0, 1, 0, 0 };

    wb_hdr(s, &b, WLAN_TYPE_MGMT, subtype, 0, dst, ap->bssid.a, ap->bssid.a);
    stq_le_p(ts, esp_wifi_tsf(s));
    wb_put(&b, ts, 8);
    wb_le16(&b, s->beacon_interval_tu);
    wb_le16(&b, 0x0401 | (ap_rsn(ap) ? 0x0010 : 0));   /* ESS, short slot */
    wb_ie(&b, WLAN_EID_SSID, ap->ssid, strlen(ap->ssid));
    wb_rates(&b);
    wb_ie(&b, WLAN_EID_DS_PARAMS, &ch, 1);
    if (subtype == WLAN_MGMT_BEACON) {
        wb_ie(&b, WLAN_EID_TIM, tim, sizeof(tim));
    }
    if (ap_rsn(ap)) {
        wb_put(&b, rsn_ie, sizeof(rsn_ie));
    }
    esp_wifi_deliver(s, b.d, b.len, ap->rssi);
}

static void ap_send_mgmt(EspWifiState *s, EspWifiAp *ap, unsigned subtype,
                         const void *body, size_t n, bool rates)
{
    WBuf b;

    wb_hdr(s, &b, WLAN_TYPE_MGMT, subtype, 0, ap->sta.a, ap->bssid.a,
           ap->bssid.a);
    wb_put(&b, body, n);
    if (rates) {
        wb_rates(&b);
    }
    esp_wifi_deliver(s, b.d, b.len, ap->rssi);
}

static void ap_send_eapol(EspWifiState *s, EspWifiAp *ap, int msg)
{
    EspWifiRsnKeys *k = &ap->keys;
    uint8_t kd[64], wrapped[72];
    size_t nkd = 0, eapol_len;
    uint16_t info;
    uint8_t *eapol;
    WBuf b;

    wb_hdr(s, &b, WLAN_TYPE_DATA, 0, WLAN_FC_FROMDS, ap->sta.a, ap->bssid.a,
           ap->bssid.a);
    if (msg == 1) {
        /* Retransmissions keep the ANonce */
        if (!ap->eapol_tries) {
            qemu_guest_getrandom_nofail(k->anonce, sizeof(k->anonce));
        }
        k->ptk_valid = false;
        info = key_version(k) | KEY_INFO_PAIRWISE | KEY_INFO_ACK;
        wb_eapol_key(&b, info, ++k->replay, k->anonce, NULL, 0, &eapol_len);
    } else {
        /* RSN IE and GTK KDE, padded and wrapped with the KEK */
        memcpy(kd, rsn_ie, sizeof(rsn_ie));
        nkd = sizeof(rsn_ie);
        kd[nkd++] = 0xdd;
        kd[nkd++] = 22;
        kd[nkd++] = 0x00;
        kd[nkd++] = 0x0f;
        kd[nkd++] = 0xac;
        kd[nkd++] = 0x01;                       /* GTK KDE */
        kd[nkd++] = 0x01;                       /* key ID 1 */
        kd[nkd++] = 0x00;
        qemu_guest_getrandom_nofail(&kd[nkd], 16);
        nkd += 16;
        kd[nkd++] = 0xdd;
        while (nkd % 8) {
            kd[nkd++] = 0;
        }
        nkd = aes_wrap(k->ptk + 16, kd, nkd, wrapped);
        info = key_version(k) | KEY_INFO_PAIRWISE | KEY_INFO_INSTALL |
               KEY_INFO_ACK | KEY_INFO_MIC | KEY_INFO_SECURE |
               KEY_INFO_ENCRYPTED;
        eapol = wb_eapol_key(&b, info, ++k->replay, k->anonce, wrapped, nkd,
                             &eapol_len);
        eapol_mic(k, eapol, eapol_len);
    }
    trace_esp_wifi_eapol("ap", msg);
    esp_wifi_deliver(s, b.d, b.len, ap->rssi);
}

static void ap_eapol_rearm(EspWifiState *s)
{
    int64_t next = INT64_MAX;
    int i;

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        if (s->ap[i].eapol_msg) {
            next = MIN(next, s->ap[i].eapol_due);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->eapol_timer);
    } else {
        timer_mod(s->eapol_timer, next);
    }
}

/*
 * Send message 1 or 3 of the 4-way handshake after @delay, and again every
 * second until the station answers, as access points do.
 */
static void ap_eapol_schedule(EspWifiState *s, EspWifiAp *ap, int msg,
                              int64_t delay)
{
    ap->eapol_msg = msg;
    ap->eapol_tries = 0;
    ap->eapol_due = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay;
    ap_eapol_rearm(s);
}

static void esp_wifi_eapol_timer(void *opaque)
{
    EspWifiState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int i;

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        EspWifiAp *ap = &s->ap[i];

        if (!ap->eapol_msg || ap->eapol_due > now) {
            continue;
        }
        if (ap->link != ESP_WIFI_LINK_HANDSHAKE ||
            ap->eapol_tries >= EAPOL_TRIES) {
            ap->eapol_msg = 0;
            continue;
        }
        ap_send_eapol(s, ap, ap->eapol_msg);
        ap->eapol_tries++;
        ap->eapol_due = now + EAPOL_RETRY_NS;
    }
    ap_eapol_rearm(s);
}

static void ap_rx_eapol(EspWifiState *s, EspWifiAp *ap, const uint8_t *eapol,
                        size_t len)
{
    EspWifiRsnKeys *k = &ap->keys;
    uint16_t info;

    if (len < EK_DATA || eapol[1] != 3 || ap->link < ESP_WIFI_LINK_HANDSHAKE) {
        return;
    }
    info = lduw_be_p(eapol + EK_INFO);
    if (!(info & KEY_INFO_MIC) || ldq_be_p(eapol + EK_REPLAY) != k->replay) {
        return;
    }
    if (!(info & KEY_INFO_SECURE)) {
        /* Message 2 */
        trace_esp_wifi_eapol("ap", 2);
        memcpy(k->snonce, eapol + EK_NONCE, 32);
        wpa_ptk(k, ap->pmk, ap->bssid.a, ap->sta.a);
        if (!eapol_mic_ok(k, eapol, len)) {
            qemu_log_mask(LOG_GUEST_ERROR, "esp_wifi: %s: 4-way handshake "
                          "MIC mismatch, wrong passphrase?\n", ap->ssid);
            return;
        }
        ap_eapol_schedule(s, ap, 3, 0);
    } else if (k->ptk_valid && eapol_mic_ok(k, eapol, len)) {
        /* Message 4 */
        trace_esp_wifi_eapol("ap", 4);
        ap->eapol_msg = 0;
        ap->link = ESP_WIFI_LINK_ESTABLISHED;
        trace_esp_wifi_link(ap->ssid, ap->link);
    }
}

static void ap_rx_mgmt(EspWifiState *s, EspWifiAp *ap, unsigned subtype,
                       const uint8_t *f, size_t len)
{
    const uint8_t *body = f + WLAN_HDR_LEN;
    size_t blen = len - WLAN_HDR_LEN;
    uint8_t resp[6];

    switch (subtype) {
    case WLAN_MGMT_AUTH:
        if (blen < 6 || lduw_le_p(body) != 0 || lduw_le_p(body + 2) != 1) {
            break;
        }
        memcpy(ap->sta.a, f + 10, 6);
        ap->link = ESP_WIFI_LINK_AUTHENTICATED;
        stw_le_p(resp, 0);              /* open system */
        stw_le_p(resp + 2, 2);
        stw_le_p(resp + 4, 0);          /* success */
        ap_send_mgmt(s, ap, WLAN_MGMT_AUTH, resp, 6, false);
        break;
    case WLAN_MGMT_ASSOC_REQ:
    case WLAN_MGMT_REASSOC_REQ:
        if (ap->link < ESP_WIFI_LINK_AUTHENTICATED ||
            !mac_eq(ap->sta.a, f + 10)) {
            break;
        }
        stw_le_p(resp, 0x0401 | (ap_rsn(ap) ? 0x0010 : 0));
        stw_le_p(resp + 2, 0);          /* success */
        stw_le_p(resp + 4, 0xc001);     /* AID 1 */
        ap_send_mgmt(s, ap, subtype == WLAN_MGMT_ASSOC_REQ ?
                     WLAN_MGMT_ASSOC_RESP : WLAN_MGMT_REASSOC_RESP,
                     resp, 6, true);
        if (ap_rsn(ap)) {
            ap->link = ESP_WIFI_LINK_HANDSHAKE;
            ap->keys.replay = 0;
            ap->keys.akm = 2;
            ap_eapol_schedule(s, ap, 1, EAPOL_FIRST_NS);
        } else {
            ap->link = ESP_WIFI_LINK_ESTABLISHED;
        }
        trace_esp_wifi_link(ap->ssid, ap->link);
        break;
    case WLAN_MGMT_DEAUTH:
    case WLAN_MGMT_DISASSOC:
        ap->link = subtype == WLAN_MGMT_DEAUTH ? ESP_WIFI_LINK_NONE
                                               : ESP_WIFI_LINK_AUTHENTICATED;
        trace_esp_wifi_link(ap->ssid, ap->link);
        break;
    }
}

static void ap_probe(EspWifiState *s, const uint8_t *f, size_t len)
{
    const uint8_t *ssid = wlan_find_ie(f + WLAN_HDR_LEN, len - WLAN_HDR_LEN,
                                       WLAN_EID_SSID);
    int i;

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        EspWifiAp *ap = &s->ap[i];

        if (!ap_on_air(s, ap)) {
            continue;
        }
        if (ssid && ssid[1] &&
            (ssid[1] != strlen(ap->ssid) || memcmp(ssid + 2, ap->ssid,
                                                   ssid[1]))) {
            continue;
        }
        ap_send_info(s, ap, f + 10, WLAN_MGMT_PROBE_RESP);
    }
}

static void esp_wifi_beacon_timer(void *opaque)
{
    EspWifiState *s = opaque;
    int i;

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        if (ap_on_air(s, &s->ap[i])) {
            ap_send_info(s, &s->ap[i], bcast, WLAN_MGMT_BEACON);
        }
    }
    timer_mod(s->beacon_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)s->beacon_interval_tu * TU_NS);
}

/* Payload of a data frame: skip the header and check the LLC/SNAP header */
static const uint8_t *data_payload(const uint8_t *f, size_t len,
                                   uint16_t *ethertype, size_t *plen)
{
    uint16_t fc = wlan_fc(f);
    size_t hdr = WLAN_HDR_LEN;

    if (WLAN_FC_SUBTYPE(fc) & WLAN_DATA_QOS) {
        hdr += 2;
    }
    if (fc & WLAN_FC_PROTECTED) {
        /* CCMP header and MIC, which the MAC fills in when encrypting */
        hdr += 8;
        len = len >= 8 ? len - 8 : 0;
    }
    if (len < hdr + LLC_SNAP_LEN || memcmp(f + hdr, llc_snap, 6)) {
        return NULL;
    }
    *ethertype = lduw_be_p(f + hdr + 6);
    *plen = len - hdr - LLC_SNAP_LEN;
    return f + hdr + LLC_SNAP_LEN;
}

static void send_eth(EspWifiState *s, const uint8_t *dst, const uint8_t *src,
                     uint16_t type, const uint8_t *payload, size_t n)
{
    uint8_t buf[ESP_WIFI_FRAME_MAX];

    if (n + ETH_HLEN > sizeof(buf)) {
        return;
    }
    memcpy(buf, dst, 6);
    memcpy(buf + 6, src, 6);
    stw_be_p(buf + 12, type);
    memcpy(buf + ETH_HLEN, payload, n);
    esp_wifi_send_eth(s, buf, n + ETH_HLEN);
}

/* Build a data frame carrying an Ethernet payload */
/*
 * Build a data frame carrying an Ethernet payload. On a protected link it is
 * laid out as the MAC hands it over after decryption: CCMP header, clear
 * text, room for the MIC. reverse-engineered
 */
static void wb_data(EspWifiState *s, WBuf *b, uint16_t flags,
                    const uint8_t *a1, const uint8_t *a2, const uint8_t *a3,
                    const uint8_t *eth, size_t len, EspWifiRsnKeys *k)
{
    static const uint8_t mic[8];
    uint8_t ccmp[8];

    wb_hdr(s, b, WLAN_TYPE_DATA, 0, flags | (k ? WLAN_FC_PROTECTED : 0),
           a1, a2, a3);
    if (k) {
        uint64_t pn = ++k->tx_pn;

        ccmp[0] = pn;
        ccmp[1] = pn >> 8;
        ccmp[2] = 0;
        ccmp[3] = 0x20 | ((wlan_is_group(a1) ? 1 : 0) << 6);   /* ExtIV */
        stl_le_p(ccmp + 4, pn >> 16);
        wb_put(b, ccmp, 8);
    }
    wb_put(b, llc_snap, 6);
    wb_put(b, eth + 12, len - 12);      /* EtherType and payload */
    if (k) {
        wb_put(b, mic, 8);
    }
}

/* Stations joining the guest's SoftAP */

static EspWifiClient *client_find(EspWifiState *s, const uint8_t *mac)
{
    int i;

    for (i = 0; i < ESP_WIFI_MAX_CLIENTS; i++) {
        if (s->client[i].used && mac_eq(s->client[i].mac.a, mac)) {
            return &s->client[i];
        }
    }
    return NULL;
}

static EspWifiClient *client_new(EspWifiState *s, const uint8_t *mac)
{
    int i;

    for (i = 0; i < ESP_WIFI_MAX_CLIENTS; i++) {
        EspWifiClient *c = &s->client[i];

        if (!c->used) {
            c->used = true;
            memcpy(c->mac.a, mac, 6);
            return c;
        }
    }
    return NULL;
}

/* RSN IE of the emulated clients, matching what the guest SoftAP offers */
static size_t client_rsn_ie(EspWifiState *s, uint8_t *ie)
{
    memcpy(ie, rsn_ie, sizeof(rsn_ie));
    ie[19] = s->guest_ap.akm;
    stw_le_p(ie + 20, s->guest_ap.rsn_caps);
    return sizeof(rsn_ie);
}

static void client_send_mgmt(EspWifiState *s, EspWifiClient *c,
                             unsigned subtype, const void *body, size_t n)
{
    EspWifiGuestAp *g = &s->guest_ap;
    WBuf b;
    uint8_t ch = g->channel;

    wb_hdr(s, &b, WLAN_TYPE_MGMT, subtype, 0, g->bssid.a, c->mac.a,
           g->bssid.a);
    wb_put(&b, body, n);
    if (subtype == WLAN_MGMT_ASSOC_REQ) {
        wb_ie(&b, WLAN_EID_SSID, g->ssid, strlen(g->ssid));
        wb_rates(&b);
        wb_ie(&b, WLAN_EID_DS_PARAMS, &ch, 1);
        if (g->rsn) {
            uint8_t ie[sizeof(rsn_ie)];

            wb_put(&b, ie, client_rsn_ie(s, ie));
        }
    }
    esp_wifi_deliver(s, b.d, b.len, AIR_RSSI);
}

static void client_flush(EspWifiState *s, EspWifiClient *c)
{
    GByteArray *eth;
    WBuf b;

    while ((eth = g_queue_pop_head(&c->pending))) {
        wb_data(s, &b, WLAN_FC_TODS, s->guest_ap.bssid.a, c->mac.a, eth->data,
                eth->data, eth->len, s->guest_ap.rsn ? &c->keys : NULL);
        esp_wifi_deliver(s, b.d, b.len, AIR_RSSI);
        g_byte_array_unref(eth);
    }
}

static void client_reset(EspWifiClient *c)
{
    g_queue_clear_full(&c->pending, (GDestroyNotify)g_byte_array_unref);
    c->used = false;
    c->link = ESP_WIFI_LINK_NONE;
    c->last_attempt = 0;
    memset(&c->keys, 0, sizeof(c->keys));
}

static void client_set_link(EspWifiState *s, EspWifiClient *c,
                            EspWifiLinkState link)
{
    c->link = link;
    trace_esp_wifi_link(s->guest_ap.ssid, link);
    if (link == ESP_WIFI_LINK_ESTABLISHED) {
        client_flush(s, c);
    }
}

static void client_kick(EspWifiState *s, EspWifiClient *c)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t auth[6];

    if (!s->guest_ap.valid || (s->guest_ap.rsn && !s->guest_ap.akm) ||
        c->link != ESP_WIFI_LINK_NONE ||
        (c->last_attempt && now - c->last_attempt < CLIENT_RETRY_NS)) {
        return;
    }
    c->last_attempt = now;
    stw_le_p(auth, 0);                  /* open system */
    stw_le_p(auth + 2, 1);
    stw_le_p(auth + 4, 0);
    client_send_mgmt(s, c, WLAN_MGMT_AUTH, auth, 6);
}

static void esp_wifi_client_timer(void *opaque)
{
    EspWifiState *s = opaque;
    int i;

    for (i = 0; i < ESP_WIFI_MAX_CLIENTS; i++) {
        if (!g_queue_is_empty(&s->client[i].pending)) {
            client_kick(s, &s->client[i]);
        }
    }
    timer_mod(s->client_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CLIENT_TICK_NS);
}

static void client_rx_mgmt(EspWifiState *s, EspWifiClient *c,
                           unsigned subtype, const uint8_t *f, size_t len)
{
    const uint8_t *body = f + WLAN_HDR_LEN;
    size_t blen = len - WLAN_HDR_LEN;
    uint8_t req[4];

    switch (subtype) {
    case WLAN_MGMT_AUTH:
        if (blen >= 6 && lduw_le_p(body + 2) == 2 && lduw_le_p(body + 4) == 0) {
            client_set_link(s, c, ESP_WIFI_LINK_AUTHENTICATED);
            stw_le_p(req, 0x0431);      /* ESS, short preamble and slot */
            stw_le_p(req + 2, 10);      /* listen interval */
            client_send_mgmt(s, c, WLAN_MGMT_ASSOC_REQ, req, 4);
        }
        break;
    case WLAN_MGMT_ASSOC_RESP:
    case WLAN_MGMT_REASSOC_RESP:
        if (blen >= 6 && lduw_le_p(body + 2) == 0) {
            client_set_link(s, c, s->guest_ap.rsn ? ESP_WIFI_LINK_HANDSHAKE
                                                  : ESP_WIFI_LINK_ESTABLISHED);
        }
        break;
    case WLAN_MGMT_DEAUTH:
    case WLAN_MGMT_DISASSOC:
        client_set_link(s, c, ESP_WIFI_LINK_NONE);
        c->last_attempt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        break;
    }
}

static void client_rx_eapol(EspWifiState *s, EspWifiClient *c,
                            const uint8_t *eapol, size_t len)
{
    EspWifiGuestAp *g = &s->guest_ap;
    EspWifiRsnKeys *k = &c->keys;
    uint8_t ie[sizeof(rsn_ie)];
    uint16_t info, reply;
    size_t eapol_len;
    uint8_t *out;
    WBuf b;

    if (len < EK_DATA || eapol[1] != 3 || c->link != ESP_WIFI_LINK_HANDSHAKE) {
        return;
    }
    info = lduw_be_p(eapol + EK_INFO);
    if (!(info & KEY_INFO_ACK)) {
        return;
    }
    k->replay = ldq_be_p(eapol + EK_REPLAY);
    wb_hdr(s, &b, WLAN_TYPE_DATA, 0, WLAN_FC_TODS, g->bssid.a, c->mac.a,
           g->bssid.a);
    if (!(info & KEY_INFO_MIC)) {
        /* Message 1: answer with message 2, carrying our RSN IE */
        trace_esp_wifi_eapol("client", 1);
        memcpy(k->anonce, eapol + EK_NONCE, 32);
        qemu_guest_getrandom_nofail(k->snonce, 32);
        k->akm = g->akm;
        wpa_ptk(k, g->pmk, g->bssid.a, c->mac.a);
        reply = key_version(k) | KEY_INFO_PAIRWISE | KEY_INFO_MIC;
        out = wb_eapol_key(&b, reply, k->replay, k->snonce, ie,
                           client_rsn_ie(s, ie), &eapol_len);
    } else {
        /* Message 3: answer with message 4 */
        trace_esp_wifi_eapol("client", 3);
        if (!k->ptk_valid || !eapol_mic_ok(k, eapol, len)) {
            qemu_log_mask(LOG_GUEST_ERROR, "esp_wifi: SoftAP 4-way handshake "
                          "MIC mismatch, check client-psk\n");
            return;
        }
        reply = key_version(k) | KEY_INFO_PAIRWISE | KEY_INFO_MIC |
                KEY_INFO_SECURE;
        out = wb_eapol_key(&b, reply, k->replay, NULL, NULL, 0, &eapol_len);
    }
    eapol_mic(k, out, eapol_len);
    esp_wifi_deliver(s, b.d, b.len, AIR_RSSI);
    if (info & KEY_INFO_MIC) {
        client_set_link(s, c, ESP_WIFI_LINK_ESTABLISHED);
    }
}

/*
 * Pick the AKM suite the emulated clients use with the guest SoftAP: PSK, or
 * PSK-SHA256 when the SoftAP requires protected management frames. Returns 0
 * if neither is offered (e.g. WPA3-SAE only).
 */
static uint8_t rsn_parse(const uint8_t *ie, uint16_t *caps)
{
    const uint8_t *p = ie + 2, *end = ie + 2 + ie[1];
    uint8_t akm = 0;
    uint16_t n, i;

    p += 2 + 4;                         /* version, group cipher */
    if (p + 2 > end) {
        return 0;
    }
    n = lduw_le_p(p);
    p += 2 + 4 * n;                     /* pairwise ciphers */
    if (p + 2 > end) {
        return 0;
    }
    n = lduw_le_p(p);
    p += 2;
    for (i = 0; i < n && p + 4 <= end; i++, p += 4) {
        if (!memcmp(p, "\x00\x0f\xac", 3) && (p[3] == 2 || p[3] == 6) &&
            (!akm || p[3] == 2)) {
            akm = p[3];
        }
    }
    *caps = p + 2 <= end ? lduw_le_p(p) & 0x00c0 : 0;  /* MFPR, MFPC */
    return akm;
}

static void guest_ap_beacon(EspWifiState *s, const uint8_t *f, size_t len)
{
    EspWifiGuestAp *g = &s->guest_ap;
    const uint8_t *ies = f + WLAN_HDR_LEN + 12;
    size_t nies = len > WLAN_HDR_LEN + 12 ? len - WLAN_HDR_LEN - 12 : 0;
    const uint8_t *ssid = wlan_find_ie(ies, nies, WLAN_EID_SSID);
    const uint8_t *ds = wlan_find_ie(ies, nies, WLAN_EID_DS_PARAMS);
    const uint8_t *rsn_ie_p = wlan_find_ie(ies, nies, WLAN_EID_RSN);
    bool rsn = rsn_ie_p != NULL;
    char name[33] = "";

    if (ssid && ssid[1] <= 32) {
        memcpy(name, ssid + 2, ssid[1]);
    }
    if (g->valid && mac_eq(g->bssid.a, f + 16) && !strcmp(g->ssid, name) &&
        g->rsn == rsn) {
        return;
    }
    memcpy(g->bssid.a, f + 16, 6);
    pstrcpy(g->ssid, sizeof(g->ssid), name);
    g->channel = ds && ds[1] ? ds[2] : s->channel;
    g->rsn = rsn;
    g->akm = rsn ? rsn_parse(rsn_ie_p, &g->rsn_caps) : 0;
    g->valid = true;
    trace_esp_wifi_guest_ap(g->ssid, g->channel, rsn, g->akm, g->rsn_caps);
    if (rsn && !g->akm) {
        qemu_log_mask(LOG_UNIMP, "esp_wifi: SoftAP '%s' offers neither "
                      "WPA2-PSK nor PSK-SHA256, its clients cannot be "
                      "emulated\n", g->ssid);
    } else if (rsn) {
        if (!s->client_psk || !wpa_pmk(s->client_psk, g->ssid, g->pmk)) {
            qemu_log_mask(LOG_GUEST_ERROR, "esp_wifi: SoftAP '%s' uses WPA2, "
                          "set the client-psk property to join it\n", g->ssid);
        }
    }
}

/* Frames tunnelled between QEMU instances */

static void air_send(EspWifiState *s, const uint8_t *f, size_t len)
{
    uint8_t buf[ESP_WIFI_FRAME_MAX];
    const uint8_t *dst = f + 4;

    if (len + ETH_HLEN + AIR_HDR_LEN > sizeof(buf)) {
        return;
    }
    memcpy(buf, wlan_is_group(dst) ? bcast : dst, 6);
    memcpy(buf + 6, len >= 16 ? f + 10 : esp_wifi_own_mac(s, 0), 6);
    stw_be_p(buf + 12, ESP_WIFI_AIR_ETHERTYPE);
    memcpy(buf + ETH_HLEN, ESP_WIFI_AIR_MAGIC, 4);
    buf[ETH_HLEN + 4] = 1;              /* version */
    buf[ETH_HLEN + 5] = s->channel;
    buf[ETH_HLEN + 6] = 0;
    buf[ETH_HLEN + 7] = 0;
    memcpy(buf + ETH_HLEN + AIR_HDR_LEN, f, len);
    trace_esp_wifi_air_tx(s->channel, len, wlan_fc(f));
    esp_wifi_send_eth(s, buf, len + ETH_HLEN + AIR_HDR_LEN);
}

static void air_rx(EspWifiState *s, const uint8_t *buf, size_t len)
{
    const uint8_t *f = buf + ETH_HLEN + AIR_HDR_LEN;
    size_t flen;
    uint16_t fc;
    int i;

    if (len < ETH_HLEN + AIR_HDR_LEN + 10 ||
        memcmp(buf + ETH_HLEN, ESP_WIFI_AIR_MAGIC, 4) ||
        buf[ETH_HLEN + 4] != 1) {
        return;
    }
    flen = len - ETH_HLEN - AIR_HDR_LEN;
    if (s->channel && buf[ETH_HLEN + 5] != s->channel) {
        return;
    }
    fc = wlan_fc(f);
    trace_esp_wifi_air_rx(buf[ETH_HLEN + 5], flen, fc);

    if (WLAN_FC_TYPE(fc) == WLAN_TYPE_CTRL &&
        WLAN_FC_SUBTYPE(fc) == WLAN_CTRL_ACK) {
        if (!is_own_mac(s, f + 4)) {
            return;
        }
        for (i = 0; i < ESP_WIFI_TX_SLOTS; i++) {
            if (s->tx[i].busy && s->tx[i].wait_ack) {
                esp_wifi_tx_complete(s, i, true);
                break;
            }
        }
        return;
    }
    if (flen < WLAN_HDR_LEN || is_own_mac(s, f + 10)) {
        return;
    }
    if (!wlan_is_group(f + 4) && !is_own_mac(s, f + 4)) {
        return;
    }
    esp_wifi_deliver(s, f, flen, AIR_RSSI);
    if (!wlan_is_group(f + 4)) {
        uint8_t ack[10] = { WLAN_TYPE_CTRL << 2 | WLAN_CTRL_ACK << 4 };

        memcpy(ack + 4, f + 10, 6);
        air_send(s, ack, sizeof(ack));
    }
}

/* Interface to the MAC core */

bool esp_wifi_air_tx(EspWifiState *s, int slot, const uint8_t *f, size_t len)
{
    uint16_t fc = wlan_fc(f);
    unsigned type = WLAN_FC_TYPE(fc), subtype = WLAN_FC_SUBTYPE(fc);
    const uint8_t *a1 = f + 4, *a2 = f + 10, *a3 = f + 16;
    EspWifiClient *c;
    EspWifiAp *ap;
    const uint8_t *payload;
    uint16_t ethertype;
    size_t plen;

    if (type == WLAN_TYPE_CTRL || len < WLAN_HDR_LEN) {
        return false;
    }

    if (type == WLAN_TYPE_MGMT) {
        if (subtype == WLAN_MGMT_PROBE_REQ) {
            ap_probe(s, f, len);
            return false;
        }
        if (subtype == WLAN_MGMT_BEACON) {
            guest_ap_beacon(s, f, len);
            return false;
        }
        ap = ap_by_bssid(s, a1);
        if (ap) {
            ap_rx_mgmt(s, ap, subtype, f, len);
            return false;
        }
        c = s->guest_ap.valid && mac_eq(a2, s->guest_ap.bssid.a) ?
            client_find(s, a1) : NULL;
        if (c) {
            client_rx_mgmt(s, c, subtype, f, len);
            return false;
        }
        if (subtype == WLAN_MGMT_ACTION) {
            air_send(s, f, len);
            return !wlan_is_group(a1);
        }
        return false;
    }

    /* Data frames */
    if (WLAN_FC_SUBTYPE(fc) & WLAN_DATA_NULL) {
        return false;
    }
    payload = data_payload(f, len, &ethertype, &plen);
    if (!payload) {
        return false;
    }
    ap = (fc & WLAN_FC_TODS) ? ap_by_bssid(s, a1) : NULL;
    if (ap) {
        if (ethertype == ETH_P_EAPOL) {
            ap_rx_eapol(s, ap, payload, plen);
        } else if (ap->link == ESP_WIFI_LINK_ESTABLISHED) {
            send_eth(s, a3, a2, ethertype, payload, plen);
        }
        return false;
    }
    if ((fc & WLAN_FC_FROMDS) && s->guest_ap.valid &&
        mac_eq(a2, s->guest_ap.bssid.a)) {
        c = client_find(s, a1);
        if (c && ethertype == ETH_P_EAPOL) {
            client_rx_eapol(s, c, payload, plen);
        } else if (wlan_is_group(a1) ||
                   (c && c->link == ESP_WIFI_LINK_ESTABLISHED)) {
            send_eth(s, a1, a3, ethertype, payload, plen);
        }
    }
    return false;
}

void esp_wifi_air_rx_eth(EspWifiState *s, const uint8_t *buf, size_t len)
{
    const uint8_t *dst = buf, *src = buf + 6;
    EspWifiClient *c;
    WBuf b;
    int i;

    if (len < ETH_HLEN || len - ETH_HLEN + WLAN_HDR_LEN + LLC_SNAP_LEN >
                          sizeof(b.d)) {
        return;
    }
    if (lduw_be_p(buf + 12) == ESP_WIFI_AIR_ETHERTYPE) {
        air_rx(s, buf, len);
        return;
    }

    /* To the guest station, through the AP it joined */
    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        EspWifiAp *ap = &s->ap[i];

        if (ap->link != ESP_WIFI_LINK_ESTABLISHED || !ap_on_air(s, ap)) {
            continue;
        }
        if (wlan_is_group(dst) || mac_eq(dst, ap->sta.a)) {
            wb_data(s, &b, WLAN_FC_FROMDS, dst, ap->bssid.a, src, buf, len,
                    ap_rsn(ap) ? &ap->keys : NULL);
            esp_wifi_deliver(s, b.d, b.len, ap->rssi);
        }
        break;
    }

    /* To the guest SoftAP, from a station emulated for the sender */
    if (!s->guest_ap.valid || wlan_is_group(src) || is_own_mac(s, src)) {
        return;
    }
    c = client_find(s, src);
    if (!c) {
        c = client_new(s, src);
        if (!c) {
            return;
        }
        trace_esp_wifi_client_new(src[0], src[1], src[2], src[3], src[4],
                                  src[5]);
    }
    if (c->link == ESP_WIFI_LINK_ESTABLISHED) {
        wb_data(s, &b, WLAN_FC_TODS, s->guest_ap.bssid.a, src, dst, buf, len,
                s->guest_ap.rsn ? &c->keys : NULL);
        esp_wifi_deliver(s, b.d, b.len, AIR_RSSI);
        return;
    }
    if (g_queue_get_length(&c->pending) < CLIENT_PENDING_MAX) {
        g_queue_push_tail(&c->pending, g_byte_array_new_take(
                              g_memdup2(buf, len), len));
    }
    client_kick(s, c);
}

void esp_wifi_air_channel_changed(EspWifiState *s)
{
    int i;

    /* Answer a passive scan quickly instead of after a beacon interval */
    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        if (ap_on_air(s, &s->ap[i])) {
            timer_mod(s->beacon_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * TU_NS);
            break;
        }
    }
}

void esp_wifi_air_reset(EspWifiState *s)
{
    int i;

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        s->ap[i].link = ESP_WIFI_LINK_NONE;
        s->ap[i].eapol_msg = 0;
        memset(&s->ap[i].keys, 0, sizeof(s->ap[i].keys));
    }
    timer_del(s->eapol_timer);
    for (i = 0; i < ESP_WIFI_MAX_CLIENTS; i++) {
        client_reset(&s->client[i]);
    }
    memset(&s->guest_ap, 0, sizeof(s->guest_ap));
    timer_mod(s->beacon_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)s->beacon_interval_tu * TU_NS);
    timer_mod(s->client_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CLIENT_TICK_NS);
}

void esp_wifi_air_realize(EspWifiState *s, Error **errp)
{
    bool any = false;
    int i;

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        EspWifiAp *ap = &s->ap[i];

        any |= ap_configured(ap);
        /* Locally administered BSSIDs */
        ap->bssid = (MACAddr) { .a = { 0x12, 0x01, 0x00, 0xc4, 0x0a,
                                       0x50 + i } };
        if (ap->channel < 1 || ap->channel > 14) {
            error_setg(errp, "ap%d-channel must be between 1 and 14", i);
            return;
        }
    }
    if (!any) {
        g_free(s->ap[0].ssid);
        s->ap[0].ssid = g_strdup("Espressif");
    }
    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        EspWifiAp *ap = &s->ap[i];

        if (ap_configured(ap) && ap_rsn(ap)) {
            if (strlen(ap->psk) < 8 || strlen(ap->psk) > 63) {
                error_setg(errp, "ap%d-psk must be 8 to 63 characters", i);
                return;
            }
            if (!wpa_pmk(ap->psk, ap->ssid, ap->pmk)) {
                error_setg(errp, "cannot derive the WPA2 key of ap%d", i);
                return;
            }
        }
    }
    s->beacon_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, esp_wifi_beacon_timer,
                                   s);
    s->client_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, esp_wifi_client_timer,
                                   s);
    s->eapol_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, esp_wifi_eapol_timer, s);
}

void esp_wifi_air_init(EspWifiState *s)
{
    int i;

    for (i = 0; i < ESP_WIFI_MAX_CLIENTS; i++) {
        g_queue_init(&s->client[i].pending);
    }
}
