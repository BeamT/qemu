/*
 * Espressif Wi-Fi MAC emulation - hardware key table and CCMP
 *
 * The MAC encrypts and decrypts CCMP (IEEE 802.11-2020 12.5.3) with keys the
 * driver installs in a key table; the 4-way handshake itself runs in
 * software. Only CCMP-128 is modelled.
 *
 * A key entry is ESP_WIFI_KEY_ENTRY_SIZE bytes. reverse-engineered, see
 * docs/wifi.md for confidence levels:
 *
 *   word 0      peer address, bytes 0-3
 *   word 1      [15:0] peer address, bytes 4-5
 *               [23]   group key           [21] pairwise key
 *               [24]   interface (0 STA, 1 SoftAP)
 *               [31:30] key ID
 *   words 2-5   key
 *
 * Bit n of the key valid register says whether entry n is in use.
 *
 * Copyright (c) 2026 Jason Kraus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "crypto/cipher.h"
#include "hw/misc/esp_wifi.h"
#include "esp_wifi_int.h"
#include "trace.h"

#define KEY_W1_GROUP        BIT(23)
#define KEY_W1_IFACE        BIT(24)
#define KEY_W1_KEYID(w)     extract32(w, 30, 2)

#define CCMP_EXT_IV         0x20

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* Length of the MAC header, up to the frame body */
size_t wlan_hdr_len(uint16_t fc)
{
    size_t n = WLAN_HDR_LEN;

    if (WLAN_FC_TYPE(fc) == WLAN_TYPE_DATA) {
        if ((fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) ==
            (WLAN_FC_TODS | WLAN_FC_FROMDS)) {
            n += 6;
        }
        if (WLAN_FC_SUBTYPE(fc) & WLAN_DATA_QOS) {
            n += 2;
            if (fc & WLAN_FC_ORDER) {
                n += 4;                 /* HT control */
            }
        }
    } else if (WLAN_FC_TYPE(fc) == WLAN_TYPE_MGMT && (fc & WLAN_FC_ORDER)) {
        n += 4;
    }
    return n;
}

/* Key table */

static uint32_t key_word(EspWifiState *s, int i, int w)
{
    const EspWifiMacLayout *l = ESP_WIFI_GET_CLASS(s)->layout;

    return s->mac_regs[(l->key_table + i * ESP_WIFI_KEY_ENTRY_SIZE) / 4 + w];
}

static bool key_valid(EspWifiState *s, int i)
{
    const EspWifiMacLayout *l = ESP_WIFI_GET_CLASS(s)->layout;

    return s->mac_regs[l->key_valid / 4] & BIT(i);
}

static void key_addr(EspWifiState *s, int i, uint8_t *addr)
{
    stl_le_p(addr, key_word(s, i, 0));
    stw_le_p(addr + 4, key_word(s, i, 1));
}

/*
 * Find the key for a frame exchanged with @peer: the pairwise key of that
 * peer, or the group key with @keyid, preferring the entries of @iface.
 */
static int key_find(EspWifiState *s, const uint8_t *peer, bool group,
                    int keyid, int iface)
{
    int i, found = -1;

    if (!ESP_WIFI_GET_CLASS(s)->layout->key_table) {
        return -1;
    }
    for (i = 0; i < ESP_WIFI_KEYS; i++) {
        uint32_t w1 = key_word(s, i, 1);
        uint8_t addr[6];

        if (!key_valid(s, i) || !(w1 & KEY_W1_GROUP) != !group) {
            continue;
        }
        key_addr(s, i, addr);
        if (group) {
            if (KEY_W1_KEYID(w1) != keyid ||
                (memcmp(addr, peer, 6) && memcmp(addr, bcast, 6))) {
                continue;
            }
        } else if (memcmp(addr, peer, 6)) {
            continue;
        }
        if (!!(w1 & KEY_W1_IFACE) == iface) {
            return i;
        }
        if (found < 0) {
            found = i;
        }
    }
    return found;
}

static QCryptoCipher *key_cipher(EspWifiState *s, int i)
{
    EspWifiKey *k = &s->keys[i];
    uint8_t key[16];
    int w;

    for (w = 0; w < 4; w++) {
        stl_le_p(key + 4 * w, key_word(s, i, 2 + w));
    }
    if (!k->aes || memcmp(key, k->aes_key, 16)) {
        qcrypto_cipher_free(k->aes);
        k->aes = qcrypto_cipher_new(QCRYPTO_CIPHER_ALGO_AES_128,
                                    QCRYPTO_CIPHER_MODE_ECB, key, 16,
                                    &error_abort);
        memcpy(k->aes_key, key, 16);
    }
    return k->aes;
}

/* The driver (re)installs a key by writing its header: restart replay checks */
void esp_wifi_keys_written(EspWifiState *s, hwaddr addr)
{
    const EspWifiMacLayout *l = ESP_WIFI_GET_CLASS(s)->layout;
    hwaddr off;

    if (!l->key_table || addr < l->key_table ||
        addr >= l->key_table + ESP_WIFI_KEYS * ESP_WIFI_KEY_ENTRY_SIZE) {
        return;
    }
    off = addr - l->key_table;
    if (off % ESP_WIFI_KEY_ENTRY_SIZE == 4) {
        memset(s->keys[off / ESP_WIFI_KEY_ENTRY_SIZE].rx_pn, 0,
               sizeof(s->keys[0].rx_pn));
    }
}

void esp_wifi_keys_reset(EspWifiState *s)
{
    int i;

    for (i = 0; i < ESP_WIFI_KEYS; i++) {
        memset(s->keys[i].rx_pn, 0, sizeof(s->keys[i].rx_pn));
    }
}

void esp_wifi_keys_free(EspWifiState *s)
{
    int i;

    for (i = 0; i < ESP_WIFI_KEYS; i++) {
        qcrypto_cipher_free(s->keys[i].aes);
        s->keys[i].aes = NULL;
    }
}

/* AES-CCM with an 8 byte MIC and a 2 byte length field (RFC 3610) */

static void aes_block(QCryptoCipher *c, uint8_t *b)
{
    qcrypto_cipher_encrypt(c, b, b, 16, &error_abort);
}

static void ccm(QCryptoCipher *c, const uint8_t *nonce, const uint8_t *aad,
                size_t naad, uint8_t *data, size_t n, uint8_t *tag,
                bool encrypt)
{
    uint8_t x[16], a[16], s[16], blk[48];
    size_t i, j, m;

    /* CBC-MAC over B0, the AAD and the plain text */
    x[0] = 0x40 | ((WLAN_CCMP_MIC_LEN - 2) / 2) << 3 | (2 - 1);
    memcpy(x + 1, nonce, 13);
    stw_be_p(x + 14, n);
    aes_block(c, x);

    assert(naad + 2 <= sizeof(blk));
    memset(blk, 0, sizeof(blk));
    stw_be_p(blk, naad);
    memcpy(blk + 2, aad, naad);
    for (i = 0; i < naad + 2; i += 16) {
        for (j = 0; j < 16; j++) {
            x[j] ^= blk[i + j];
        }
        aes_block(c, x);
    }

    a[0] = 2 - 1;
    memcpy(a + 1, nonce, 13);
    for (i = 0; i < n; i += 16) {
        m = MIN(16, n - i);
        stw_be_p(a + 14, i / 16 + 1);
        memcpy(s, a, 16);
        aes_block(c, s);
        for (j = 0; j < m; j++) {
            if (encrypt) {
                x[j] ^= data[i + j];
                data[i + j] ^= s[j];
            } else {
                data[i + j] ^= s[j];
                x[j] ^= data[i + j];
            }
        }
        aes_block(c, x);
    }

    stw_be_p(a + 14, 0);
    memcpy(s, a, 16);
    aes_block(c, s);
    for (j = 0; j < WLAN_CCMP_MIC_LEN; j++) {
        tag[j] = x[j] ^ s[j];
    }
}

/* CCMP framing */

typedef struct CcmpFrame {
    size_t hdr;                 /* MAC header length */
    uint64_t pn;
    int keyid;
    int tid;                    /* replay counter: TID, or 16 for management */
    uint8_t nonce[13];
    uint8_t aad[30];
    size_t naad;
} CcmpFrame;

static bool ccmp_parse(const uint8_t *f, size_t len, CcmpFrame *c)
{
    uint16_t fc = wlan_fc(f), mfc;
    unsigned type = WLAN_FC_TYPE(fc);
    bool qos = type == WLAN_TYPE_DATA && (WLAN_FC_SUBTYPE(fc) & WLAN_DATA_QOS);
    bool a4 = type == WLAN_TYPE_DATA &&
              (fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) ==
              (WLAN_FC_TODS | WLAN_FC_FROMDS);
    const uint8_t *h;
    int prio = 0;
    int i;

    if (type == WLAN_TYPE_CTRL) {
        return false;
    }
    c->hdr = wlan_hdr_len(fc);
    if (len < c->hdr + WLAN_CCMP_HDR_LEN + WLAN_CCMP_MIC_LEN) {
        return false;
    }
    h = f + c->hdr;
    if (!(h[3] & CCMP_EXT_IV)) {
        return false;
    }
    c->keyid = h[3] >> 6;
    c->pn = (uint64_t)h[0] | (uint64_t)h[1] << 8 | (uint64_t)h[4] << 16 |
            (uint64_t)h[5] << 24 | (uint64_t)h[6] << 32 |
            (uint64_t)h[7] << 40;
    if (qos) {
        prio = f[WLAN_HDR_LEN + (a4 ? 6 : 0)] & 0xf;
    }
    c->tid = type == WLAN_TYPE_MGMT ? 16 : prio;

    /* Nonce: flags (priority, management), A2, PN5..PN0 */
    c->nonce[0] = prio | (type == WLAN_TYPE_MGMT ? 0x10 : 0);
    memcpy(c->nonce + 1, f + 10, 6);
    for (i = 0; i < 6; i++) {
        c->nonce[7 + i] = c->pn >> (8 * (5 - i));
    }

    /* AAD: masked FC, A1-A3, masked SC, A4, masked QC */
    mfc = fc & ~(WLAN_FC_RETRY | WLAN_FC_PWRMGT | WLAN_FC_MOREDATA);
    if (type == WLAN_TYPE_DATA) {
        mfc &= ~0x0070;
    }
    if (qos) {
        mfc &= ~WLAN_FC_ORDER;
    }
    mfc |= WLAN_FC_PROTECTED;
    stw_le_p(c->aad, mfc);
    memcpy(c->aad + 2, f + 4, 18);
    stw_le_p(c->aad + 20, lduw_le_p(f + 22) & 0xf);
    c->naad = 22;
    if (a4) {
        memcpy(c->aad + c->naad, f + 24, 6);
        c->naad += 6;
    }
    if (qos) {
        c->aad[c->naad++] = prio;
        c->aad[c->naad++] = 0;
    }
    return true;
}

static int frame_iface(EspWifiState *s, const uint8_t *addr)
{
    return !memcmp(addr, esp_wifi_own_mac(s, 1), 6) ? 1 : 0;
}

bool esp_wifi_ccmp_encrypt(EspWifiState *s, uint8_t *f, size_t len)
{
    const uint8_t *a1 = f + 4;
    CcmpFrame c;
    int k;

    if (!ccmp_parse(f, len, &c)) {
        return false;
    }
    k = key_find(s, a1, wlan_is_group(a1), c.keyid, frame_iface(s, f + 10));
    trace_esp_wifi_ccmp("tx", k, c.keyid, c.pn, true);
    if (k < 0) {
        return false;
    }
    ccm(key_cipher(s, k), c.nonce, c.aad, c.naad,
        f + c.hdr + WLAN_CCMP_HDR_LEN,
        len - c.hdr - WLAN_CCMP_HDR_LEN - WLAN_CCMP_MIC_LEN,
        f + len - WLAN_CCMP_MIC_LEN, true);
    return true;
}

bool esp_wifi_ccmp_decrypt(EspWifiState *s, uint8_t *f, size_t len)
{
    const uint8_t *a1 = f + 4, *a2 = f + 10;
    uint8_t tag[WLAN_CCMP_MIC_LEN];
    CcmpFrame c;
    bool ok;
    int k;

    if (!ccmp_parse(f, len, &c)) {
        return false;
    }
    k = key_find(s, a2, wlan_is_group(a1), c.keyid,
                 wlan_is_group(a1) ? 0 : frame_iface(s, a1));
    if (k < 0) {
        trace_esp_wifi_ccmp("rx", k, c.keyid, c.pn, false);
        return false;
    }
    if (c.pn <= s->keys[k].rx_pn[c.tid]) {
        trace_esp_wifi_ccmp_replay(k, c.tid, c.pn, s->keys[k].rx_pn[c.tid]);
        return false;
    }
    ccm(key_cipher(s, k), c.nonce, c.aad, c.naad,
        f + c.hdr + WLAN_CCMP_HDR_LEN,
        len - c.hdr - WLAN_CCMP_HDR_LEN - WLAN_CCMP_MIC_LEN, tag, false);
    ok = !memcmp(tag, f + len - WLAN_CCMP_MIC_LEN, WLAN_CCMP_MIC_LEN);
    trace_esp_wifi_ccmp("rx", k, c.keyid, c.pn, ok);
    if (ok) {
        s->keys[k].rx_pn[c.tid] = c.pn;
    }
    return ok;
}
