/*
 * Espressif Wi-Fi MAC emulation - wmediumd as the wireless medium
 *
 * With the "air" property set to a chardev connected to the API socket of
 * wmediumd (its -a option), the emulated chip becomes one more radio on the
 * medium that wmediumd simulates for mac80211_hwsim radios, vhost-user
 * clients (e.g. Android Cuttlefish) and other API clients. The emulated
 * access points, SoftAP clients and ESP-NOW tunnel of esp_wifi_air.c are
 * then not used.
 *
 * The API socket carries messages made of a header (type, data length, both
 * u32 in host order) and data. Generic netlink messages of the
 * mac80211_hwsim family travel in WMEDIUMD_MSG_NETLINK messages:
 *
 *  - every frame the guest transmits goes out as HWSIM_CMD_FRAME. wmediumd
 *    answers with HWSIM_CMD_TX_INFO_FRAME, whose flags say whether the frame
 *    was acknowledged; that completes the TX slot.
 *  - frames for this radio arrive as HWSIM_CMD_FRAME, with the frequency
 *    and the signal strength.
 *
 * wmediumd waits for a WMEDIUMD_MSG_ACK after each message it sends, and
 * answers each message from the client with one.
 *
 * wmediumd only knows the stations listed in its configuration. The frames
 * of both interfaces are sent as coming from one station, the radio, whose
 * address is the STA MAC address (the base MAC address); the SoftAP address
 * is added to that station with HWSIM_CMD_ADD_MAC_ADDR, as mac80211_hwsim
 * does for its additional interfaces. Only the STA address needs to be in
 * the configuration.
 *
 * Copyright (c) 2026 Jason Kraus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/misc/esp_wifi.h"
#include "esp_wifi_int.h"
#include "trace.h"

/* wmediumd/api.h */
enum {
    WMD_MSG_INVALID = 0,
    WMD_MSG_ACK = 1,
    WMD_MSG_REGISTER = 2,
    WMD_MSG_UNREGISTER = 3,
    WMD_MSG_NETLINK = 4,
    WMD_MSG_SET_CONTROL = 5,
    WMD_MSG_TX_START = 6,
};
#define WMD_HDR_LEN             8
#define WMD_MSG_MAX             (1024 * 1024)

/* include/uapi/linux/mac80211_hwsim.h (drivers/net/wireless/virtual) */
enum {
    HWSIM_CMD_FRAME = 2,
    HWSIM_CMD_TX_INFO_FRAME = 3,
    HWSIM_CMD_ADD_MAC_ADDR = 7,
    HWSIM_CMD_DEL_MAC_ADDR = 8,
};
enum {
    HWSIM_ATTR_ADDR_RECEIVER = 1,
    HWSIM_ATTR_ADDR_TRANSMITTER = 2,
    HWSIM_ATTR_FRAME = 3,
    HWSIM_ATTR_FLAGS = 4,
    HWSIM_ATTR_RX_RATE = 5,
    HWSIM_ATTR_SIGNAL = 6,
    HWSIM_ATTR_TX_INFO = 7,
    HWSIM_ATTR_COOKIE = 8,
    HWSIM_ATTR_FREQ = 19,
};
#define HWSIM_TX_CTL_REQ_TX_STATUS  BIT(0)
#define HWSIM_TX_CTL_NO_ACK         BIT(1)
#define HWSIM_TX_STAT_ACK           BIT(2)
#define HWSIM_TX_MAX_RATES          4

/* Netlink framing, in host byte order */
#define NLMSG_HDR_LEN           16
#define GENL_HDR_LEN            4
#define NLA_HDR_LEN             4
#define NLA_ALIGN(n)            ROUND_UP(n, 4)
#define NLA_TYPE_MASK           0x3fff
#define NLM_F_REQUEST           1

/* Unicast frames: the hardware retries, as the rate table tells wmediumd */
#define TX_RETRIES              4

typedef struct NlMsg {
    uint8_t d[ESP_WIFI_FRAME_MAX + 128];
    size_t len;
} NlMsg;

static void nl_init(NlMsg *m, uint8_t cmd)
{
    memset(m->d, 0, NLMSG_HDR_LEN + GENL_HDR_LEN);
    /* wmediumd ignores the family id */
    stw_he_p(m->d + 6, NLM_F_REQUEST);
    m->d[NLMSG_HDR_LEN] = cmd;
    m->d[NLMSG_HDR_LEN + 1] = 1;            /* version */
    m->len = NLMSG_HDR_LEN + GENL_HDR_LEN;
}

static void nl_put(NlMsg *m, uint16_t type, const void *data, size_t n)
{
    assert(m->len + NLA_HDR_LEN + NLA_ALIGN(n) <= sizeof(m->d));
    stw_he_p(m->d + m->len, NLA_HDR_LEN + n);
    stw_he_p(m->d + m->len + 2, type);
    memcpy(m->d + m->len + NLA_HDR_LEN, data, n);
    memset(m->d + m->len + NLA_HDR_LEN + n, 0, NLA_ALIGN(n) - n);
    m->len += NLA_HDR_LEN + NLA_ALIGN(n);
}

static void nl_put_u32(NlMsg *m, uint16_t type, uint32_t v)
{
    uint8_t b[4];

    stl_he_p(b, v);
    nl_put(m, type, b, 4);
}

static void nl_put_u64(NlMsg *m, uint16_t type, uint64_t v)
{
    uint8_t b[8];

    stq_he_p(b, v);
    nl_put(m, type, b, 8);
}

/* Talking to wmediumd */

static void wmd_send(EspWifiState *s, uint32_t type, const void *data,
                     uint32_t len)
{
    uint8_t hdr[WMD_HDR_LEN];

    if (!s->wmd.connected) {
        return;
    }
    stl_he_p(hdr, type);
    stl_he_p(hdr + 4, len);
    if (qemu_chr_fe_write_all(&s->wmd.chr, hdr, sizeof(hdr)) != sizeof(hdr) ||
        (len && qemu_chr_fe_write_all(&s->wmd.chr, data, len) != len)) {
        warn_report_once("esp_wifi: cannot write to the wmediumd socket");
    }
}

static void wmd_send_nl(EspWifiState *s, NlMsg *m)
{
    stl_he_p(m->d, m->len);
    wmd_send(s, WMD_MSG_NETLINK, m->d, m->len);
}

static uint32_t chan_to_freq(uint32_t ch)
{
    if (ch == 14) {
        return 2484;
    }
    return 2407 + 5 * (ch ? ch : 1);
}

static uint32_t freq_to_chan(uint32_t freq)
{
    if (freq == 2484) {
        return 14;
    }
    if (freq >= 2412 && freq <= 2472) {
        return (freq - 2407) / 5;
    }
    return 0;
}

static bool is_zero_mac(const uint8_t *a)
{
    static const uint8_t zero[6];

    return !memcmp(a, zero, 6);
}

static bool is_own(EspWifiState *s, const uint8_t *a)
{
    return !is_zero_mac(a) && (!memcmp(a, esp_wifi_own_mac(s, 0), 6) ||
                               !memcmp(a, esp_wifi_own_mac(s, 1), 6) ||
                               !memcmp(a, s->wmd.radio, 6));
}

static void wmd_mac_addr(EspWifiState *s, uint8_t cmd, const uint8_t *addr)
{
    NlMsg m;

    trace_esp_wifi_wmd_addr(cmd == HWSIM_CMD_ADD_MAC_ADDR, addr[0], addr[1],
                            addr[2], addr[3], addr[4], addr[5]);
    nl_init(&m, cmd);
    nl_put(&m, HWSIM_ATTR_ADDR_TRANSMITTER, s->wmd.radio, 6);
    nl_put(&m, HWSIM_ATTR_ADDR_RECEIVER, addr, 6);
    wmd_send_nl(s, &m);
}

/*
 * The radio address is the first interface address the driver programs.
 * Keep the station's extra addresses in step with the interface addresses.
 */
static void wmd_update_addrs(EspWifiState *s)
{
    EspWifiWmd *w = &s->wmd;
    int i;

    if (is_zero_mac(w->radio)) {
        for (i = 0; i < 2 && is_zero_mac(w->radio); i++) {
            memcpy(w->radio, esp_wifi_own_mac(s, i), 6);
        }
        if (is_zero_mac(w->radio)) {
            return;
        }
    }
    for (i = 0; i < 2; i++) {
        const uint8_t *a = esp_wifi_own_mac(s, i);
        bool want = !is_zero_mac(a) && memcmp(a, w->radio, 6);

        if (w->extra_valid[i] && (!want || memcmp(a, w->extra[i], 6))) {
            wmd_mac_addr(s, HWSIM_CMD_DEL_MAC_ADDR, w->extra[i]);
            w->extra_valid[i] = false;
        }
        if (want && !w->extra_valid[i]) {
            memcpy(w->extra[i], a, 6);
            w->extra_valid[i] = true;
            wmd_mac_addr(s, HWSIM_CMD_ADD_MAC_ADDR, a);
        }
    }
}

/*
 * 40 MHz channels. The radio is tuned to the centre of the pair, which is
 * what the PLL registers give, two channels away from the primary channel on
 * which the BSS's frames travel (IEEE 802.11-2020 19.3.15.4). Learn the
 * HT40 BSSes from their beacons and probe responses, ours included.
 */
#define WLAN_EID_HT_OPERATION   61

static void ht40_learn(EspWifiState *s, const uint8_t *f, size_t len)
{
    EspWifiWmd *w = &s->wmd;
    uint16_t fc = wlan_fc(f);
    size_t i = WLAN_HDR_LEN + 12;
    const uint8_t *ht = NULL;
    EspWifiHt40 *e = NULL;
    int centre = 0;
    unsigned j;

    if (WLAN_FC_TYPE(fc) != WLAN_TYPE_MGMT ||
        (WLAN_FC_SUBTYPE(fc) != WLAN_MGMT_BEACON &&
         WLAN_FC_SUBTYPE(fc) != WLAN_MGMT_PROBE_RESP)) {
        return;
    }
    while (i + 2 <= len && i + 2 + f[i + 1] <= len) {
        if (f[i] == WLAN_EID_HT_OPERATION && f[i + 1] >= 2) {
            ht = f + i + 2;
            break;
        }
        i += 2 + f[i + 1];
    }
    if (ht && (ht[1] & 0x4)) {          /* STA channel width: any */
        if ((ht[1] & 3) == 1) {
            centre = ht[0] + 2;         /* secondary channel above */
        } else if ((ht[1] & 3) == 3) {
            centre = ht[0] - 2;         /* below */
        }
    }
    for (j = 0; j < ESP_WIFI_HT40_MAX; j++) {
        if (w->ht40[j].primary && !memcmp(w->ht40[j].bssid, f + 16, 6)) {
            e = &w->ht40[j];
        }
    }
    if (!centre || centre < 1 || centre > 14) {
        if (e) {
            e->primary = 0;
        }
        return;
    }
    if (!e) {
        e = &w->ht40[w->ht40_next++ % ESP_WIFI_HT40_MAX];
        memcpy(e->bssid, f + 16, 6);
    }
    e->primary = ht[0];
    e->centre = centre;
}

/*
 * The channel a frame @f travels on. Tuned to the centre of a 40 MHz pair,
 * the radio sends 20 MHz frames on the pair's primary channel: those of the
 * 40 MHz BSS they are addressed to, and unicast frames to any other peer,
 * such as the station's to a 20 MHz access point on the channel where the
 * SoftAP runs HT40 (the primary is then our SoftAP's, else that of any
 * 40 MHz BSS centred there). Group frames of no such BSS, i.e. the probe
 * requests of a scan, which tunes 20 MHz wide and may well land on a centre
 * channel, and frames when no pair is known go on the radio's channel.
 */
static uint32_t frame_channel(EspWifiState *s, const uint8_t *f, size_t len)
{
    const EspWifiHt40 *pair = NULL;
    unsigned j;
    int a;

    for (j = 0; j < ESP_WIFI_HT40_MAX; j++) {
        const EspWifiHt40 *e = &s->wmd.ht40[j];

        if (!e->primary || e->centre != s->channel) {
            continue;
        }
        for (a = 4; a + 6 <= len && a <= 16; a += 6) {
            if (!memcmp(f + a, e->bssid, 6)) {
                return e->primary;
            }
        }
        if (!pair || !memcmp(e->bssid, esp_wifi_own_mac(s, 1), 6)) {
            pair = e;
        }
    }
    return pair && len >= 10 && !wlan_is_group(f + 4) ? pair->primary
                                                       : s->channel;
}

/* Does the radio hear @ch: its channel, or either half of a 40 MHz pair */
static bool hears(EspWifiState *s, uint32_t ch, const uint8_t *f,
                  size_t len)
{
    unsigned j;

    if (ch == frame_channel(s, f, len) || ch == s->channel) {
        return true;
    }
    for (j = 0; j < ESP_WIFI_HT40_MAX; j++) {
        const EspWifiHt40 *e = &s->wmd.ht40[j];

        if (e->primary && e->centre == s->channel &&
            (ch == e->centre - 2 || ch == e->centre + 2)) {
            return true;
        }
    }
    return false;
}

/*
 * Control frames that travel on the medium: block ack requests and block
 * acks, which keep the reorder windows of the two ends in step, and PS-Poll,
 * with which a dozing station fetches what its AP buffered. ACK, RTS and
 * CTS are left to wmediumd, which decides delivery itself.
 */
#define WLAN_CTRL_BAR           8
#define WLAN_CTRL_BA            9
#define WLAN_CTRL_PSPOLL        10
#define WLAN_CTRL_MIN_LEN       16

static bool ctrl_on_air(uint16_t fc)
{
    unsigned sub = WLAN_FC_SUBTYPE(fc);

    return sub == WLAN_CTRL_BAR || sub == WLAN_CTRL_BA ||
           sub == WLAN_CTRL_PSPOLL;
}

/* Long enough for its type, with a transmitter address */
static bool frame_ok(uint16_t fc, size_t len)
{
    if (WLAN_FC_TYPE(fc) == WLAN_TYPE_CTRL) {
        return ctrl_on_air(fc) && len >= WLAN_CTRL_MIN_LEN;
    }
    return len >= WLAN_HDR_LEN;
}

/* TX */

/* Hand a frame to wmediumd; the TX status comes back with @cookie */
static void wmd_send_frame(EspWifiState *s, const uint8_t *f, size_t len,
                           uint64_t cookie, int slot)
{
    bool group = wlan_is_group(f + 4);
    uint8_t rates[HWSIM_TX_MAX_RATES * 2];
    uint32_t freq = chan_to_freq(frame_channel(s, f, len));
    NlMsg m;
    int i;

    rates[0] = 0;
    rates[1] = group ? 1 : TX_RETRIES;
    for (i = 1; i < HWSIM_TX_MAX_RATES; i++) {
        rates[2 * i] = (uint8_t)-1;
        rates[2 * i + 1] = 0;
    }
    nl_init(&m, HWSIM_CMD_FRAME);
    nl_put(&m, HWSIM_ATTR_ADDR_TRANSMITTER, s->wmd.radio, 6);
    nl_put(&m, HWSIM_ATTR_FRAME, f, len);
    nl_put_u32(&m, HWSIM_ATTR_FLAGS, HWSIM_TX_CTL_REQ_TX_STATUS |
                                     (group ? HWSIM_TX_CTL_NO_ACK : 0));
    nl_put(&m, HWSIM_ATTR_TX_INFO, rates, sizeof(rates));
    nl_put_u64(&m, HWSIM_ATTR_COOKIE, cookie);
    nl_put_u32(&m, HWSIM_ATTR_FREQ, freq);
    trace_esp_wifi_wmd_tx(slot, cookie, len, wlan_fc(f), freq);
    wmd_send_nl(s, &m);
}

static bool is_beacon(const uint8_t *f, size_t len)
{
    uint16_t fc = wlan_fc(f);

    return len >= WLAN_HDR_LEN + 12 && WLAN_FC_TYPE(fc) == WLAN_TYPE_MGMT &&
           WLAN_FC_SUBTYPE(fc) == WLAN_MGMT_BEACON;
}

static int64_t beacon_interval_ns(const uint8_t *f)
{
    uint16_t tu = lduw_le_p(f + WLAN_HDR_LEN + 8);

    return (int64_t)(tu ? tu : 100) * 1024 * SCALE_US;
}

/*
 * The SoftAP's beacons. The driver hands the MAC a beacon only about once a
 * second, presumably expecting the hardware to repeat it at every target
 * beacon transmission time, which is not modelled. Peers that scan actively
 * on a real medium only listen for a few tens of milliseconds per channel,
 * so repeat the last beacon at its beacon interval, for as long as the
 * driver keeps sending new ones, and only while the radio is on the
 * SoftAP's channel (not while the station scans elsewhere).
 */
#define BEACON_STALE_NS     (3 * NANOSECONDS_PER_SECOND)

static void wmd_beacon_timer(void *opaque)
{
    EspWifiState *s = opaque;
    EspWifiWmd *w = &s->wmd;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t interval = beacon_interval_ns(w->beacon);
    uint8_t f[ESP_WIFI_FRAME_MAX];

    if (!w->connected || !w->beacon_len ||
        now - w->beacon_last > BEACON_STALE_NS ||
        memcmp(w->beacon + 10, esp_wifi_own_mac(s, 1), 6)) {
        w->beacon_len = 0;
        return;
    }
    if (now - w->beacon_last >= interval / 2 &&
        frame_channel(s, w->beacon, w->beacon_len) == w->beacon_chan) {
        memcpy(f, w->beacon, w->beacon_len);
        stw_le_p(f + 22, (s->seq++ & 0xfff) << 4);
        stq_le_p(f + WLAN_HDR_LEN, esp_wifi_tsf(s));
        wmd_send_frame(s, f, w->beacon_len, 0, -1);
    }
    timer_mod(w->beacon_timer, now + interval);
}

static void wmd_beacon_sent(EspWifiState *s, const uint8_t *f, size_t len)
{
    EspWifiWmd *w = &s->wmd;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (len > sizeof(w->beacon)) {
        return;
    }
    memcpy(w->beacon, f, len);
    w->beacon_len = len;
    w->beacon_last = now;
    w->beacon_chan = frame_channel(s, f, len);
    if (!timer_pending(w->beacon_timer)) {
        timer_mod(w->beacon_timer, now + beacon_interval_ns(f));
    }
}

void esp_wifi_wmd_tx(EspWifiState *s, int slot, const uint8_t *frame,
                     size_t len)
{
    uint16_t fc = wlan_fc(frame);
    uint8_t f[ESP_WIFI_FRAME_MAX];

    if (!frame_ok(fc, len)) {
        return;
    }
    if (!s->wmd.connected) {
        s->tx[slot].ok = false;
        return;
    }
    memcpy(f, frame, len);
    if ((fc & WLAN_FC_PROTECTED) && !esp_wifi_ccmp_encrypt(s, f, len)) {
        qemu_log_mask(LOG_GUEST_ERROR, "esp_wifi: no key to encrypt a "
                      "protected frame, dropped\n");
        s->tx[slot].ok = false;
        return;
    }
    wmd_update_addrs(s);
    ht40_learn(s, f, len);
    if (is_zero_mac(s->wmd.radio)) {
        s->tx[slot].ok = false;
        return;
    }
    /*
     * Power save is not modelled: the radio hears everything, also while
     * the driver believes it dozes, and could not wake for a beacon since
     * the TBTT timer is not modelled either. So peers must not buffer
     * frames for it. The power management bit is not covered by the MIC.
     */
    if (WLAN_FC_TYPE(fc) != WLAN_TYPE_CTRL && (fc & WLAN_FC_PWRMGT)) {
        stw_le_p(f, fc & ~WLAN_FC_PWRMGT);
    }
    /* The MAC inserts the TSF into beacons and probe responses */
    if (WLAN_FC_TYPE(fc) == WLAN_TYPE_MGMT && len >= WLAN_HDR_LEN + 8 &&
        (WLAN_FC_SUBTYPE(fc) == WLAN_MGMT_BEACON ||
         WLAN_FC_SUBTYPE(fc) == WLAN_MGMT_PROBE_RESP)) {
        stq_le_p(f + WLAN_HDR_LEN, esp_wifi_tsf(s));
    }
    if (is_beacon(f, len) && !memcmp(f + 10, esp_wifi_own_mac(s, 1), 6)) {
        wmd_beacon_sent(s, f, len);
    }
    s->tx[slot].cookie = ++s->wmd.cookie;
    s->tx[slot].pending++;
    wmd_send_frame(s, f, len, s->tx[slot].cookie, slot);
}

static void wmd_tx_info(EspWifiState *s, uint32_t flags, uint64_t cookie)
{
    EspWifiTxSlot *t;
    int i;

    for (i = 0; i < ESP_WIFI_TX_SLOTS; i++) {
        t = &s->tx[i];
        /* The MPDUs of a slot have the cookies up to t->cookie */
        if (t->busy && t->wait_ack && t->pending &&
            cookie <= t->cookie && cookie > t->cookie - t->pending) {
            trace_esp_wifi_wmd_tx_info(i, cookie, flags);
            if (!(flags & HWSIM_TX_STAT_ACK)) {
                t->ok = false;
                if (t->n_ampdu) {
                    t->ba &= ~BIT(t->n_ampdu - 1 - (t->cookie - cookie));
                }
            }
            /* Answers come in order: forget the MPDUs before this one */
            t->pending = t->cookie - cookie;
            if (!t->pending) {
                esp_wifi_tx_complete(s, i, t->ok);
            }
            return;
        }
    }
    trace_esp_wifi_wmd_tx_info(-1, cookie, flags);
}

/* RX */

static void wmd_rx_frame(EspWifiState *s, const uint8_t *receiver,
                         const uint8_t *frame, size_t len, uint32_t freq,
                         int32_t signal)
{
    uint8_t f[ESP_WIFI_FRAME_MAX];
    uint32_t ch = freq_to_chan(freq);
    uint16_t fc;

    if (len < 2 || len > sizeof(f)) {
        return;
    }
    fc = wlan_fc(frame);
    if (!frame_ok(fc, len)) {
        return;
    }
    trace_esp_wifi_wmd_rx(len, fc, freq, signal);
    if (receiver && !is_own(s, receiver)) {
        return;
    }
    if (is_own(s, frame + 10)) {
        return;
    }
    /* The radio only hears its own channel */
    if (s->channel) {
        ht40_learn(s, frame, len);
        if (!hears(s, ch, frame, len)) {
            return;
        }
    }
    /* Address filter: own addresses and group addresses */
    if (!wlan_is_group(frame + 4) && !is_own(s, frame + 4)) {
        return;
    }
    memcpy(f, frame, len);
    if ((fc & WLAN_FC_PROTECTED) && !esp_wifi_ccmp_decrypt(s, f, len)) {
        return;
    }
    esp_wifi_deliver(s, f, len, MAX(-128, MIN(signal, 0)));
}

static void wmd_rx_netlink(EspWifiState *s, const uint8_t *d, size_t len)
{
    const uint8_t *receiver = NULL, *frame = NULL;
    size_t frame_len = 0, off, msg_len;
    uint32_t freq = 0, flags = 0;
    int32_t signal = -50;
    uint64_t cookie = 0;
    uint8_t cmd;

    if (len < NLMSG_HDR_LEN + GENL_HDR_LEN) {
        return;
    }
    msg_len = MIN(ldl_he_p(d), len);
    cmd = d[NLMSG_HDR_LEN];
    for (off = NLMSG_HDR_LEN + GENL_HDR_LEN; off + NLA_HDR_LEN <= msg_len;) {
        uint16_t nla_len = lduw_he_p(d + off);
        uint16_t type = lduw_he_p(d + off + 2) & NLA_TYPE_MASK;
        const uint8_t *p = d + off + NLA_HDR_LEN;
        size_t n = nla_len - NLA_HDR_LEN;

        if (nla_len < NLA_HDR_LEN || off + nla_len > msg_len) {
            break;
        }
        switch (type) {
        case HWSIM_ATTR_ADDR_RECEIVER:
            receiver = n >= 6 ? p : NULL;
            break;
        case HWSIM_ATTR_FRAME:
            frame = p;
            frame_len = n;
            break;
        case HWSIM_ATTR_FLAGS:
            flags = n >= 4 ? ldl_he_p(p) : 0;
            break;
        case HWSIM_ATTR_SIGNAL:
            signal = n >= 4 ? (int32_t)ldl_he_p(p) : signal;
            break;
        case HWSIM_ATTR_FREQ:
            freq = n >= 4 ? ldl_he_p(p) : 0;
            break;
        case HWSIM_ATTR_COOKIE:
            cookie = n >= 8 ? ldq_he_p(p) : 0;
            break;
        }
        off += NLA_ALIGN(nla_len);
    }

    if (cmd == HWSIM_CMD_TX_INFO_FRAME) {
        wmd_tx_info(s, flags, cookie);
    } else if (cmd == HWSIM_CMD_FRAME && frame) {
        wmd_rx_frame(s, receiver, frame, frame_len, freq, signal);
    }
}

static void wmd_rx_msg(EspWifiState *s, uint32_t type, const uint8_t *data,
                       uint32_t len)
{
    switch (type) {
    case WMD_MSG_NETLINK:
        wmd_rx_netlink(s, data, len);
        wmd_send(s, WMD_MSG_ACK, NULL, 0);
        break;
    case WMD_MSG_TX_START:
        wmd_send(s, WMD_MSG_ACK, NULL, 0);
        break;
    case WMD_MSG_ACK:
        /* Answer to one of our messages */
        break;
    case WMD_MSG_INVALID:
        warn_report_once("esp_wifi: wmediumd rejected a message");
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "esp_wifi: unknown wmediumd message %"
                      PRIu32 "\n", type);
        break;
    }
}

static int wmd_can_receive(void *opaque)
{
    return 4096;
}

static void wmd_receive(void *opaque, const uint8_t *buf, int size)
{
    EspWifiState *s = opaque;
    GByteArray *rx = s->wmd.rx;
    size_t used = 0;

    g_byte_array_append(rx, buf, size);
    while (rx->len - used >= WMD_HDR_LEN) {
        uint32_t type = ldl_he_p(rx->data + used);
        uint32_t len = ldl_he_p(rx->data + used + 4);

        if (len > WMD_MSG_MAX) {
            error_report("esp_wifi: bad message from wmediumd, "
                         "disconnecting");
            g_byte_array_set_size(rx, 0);
            qemu_chr_fe_disconnect(&s->wmd.chr);
            return;
        }
        if (rx->len - used < WMD_HDR_LEN + len) {
            break;
        }
        wmd_rx_msg(s, type, rx->data + used + WMD_HDR_LEN, len);
        used += WMD_HDR_LEN + len;
    }
    g_byte_array_remove_range(rx, 0, used);
}

static void wmd_abort_tx(EspWifiState *s)
{
    int i;

    for (i = 0; i < ESP_WIFI_TX_SLOTS; i++) {
        if (s->tx[i].busy && s->tx[i].wait_ack) {
            esp_wifi_tx_complete(s, i, false);
        }
    }
}

static void wmd_event(void *opaque, QEMUChrEvent event)
{
    EspWifiState *s = opaque;
    EspWifiWmd *w = &s->wmd;

    switch (event) {
    case CHR_EVENT_OPENED:
        trace_esp_wifi_wmd_connected(true);
        w->connected = true;
        g_byte_array_set_size(w->rx, 0);
        /* A new connection: wmediumd knows nothing about us */
        memset(w->radio, 0, sizeof(w->radio));
        memset(w->extra_valid, 0, sizeof(w->extra_valid));
        wmd_send(s, WMD_MSG_REGISTER, NULL, 0);
        break;
    case CHR_EVENT_CLOSED:
        trace_esp_wifi_wmd_connected(false);
        w->connected = false;
        wmd_abort_tx(s);
        break;
    default:
        break;
    }
}

void esp_wifi_wmd_reset(EspWifiState *s)
{
    EspWifiWmd *w = &s->wmd;
    int i;

    /*
     * The driver programs its addresses again after a reset; drop the ones
     * added for the previous run so a changed SoftAP address takes over.
     */
    for (i = 0; i < 2; i++) {
        if (w->extra_valid[i]) {
            wmd_mac_addr(s, HWSIM_CMD_DEL_MAC_ADDR, w->extra[i]);
            w->extra_valid[i] = false;
        }
    }
    memset(w->radio, 0, sizeof(w->radio));
    memset(w->ht40, 0, sizeof(w->ht40));
    w->beacon_len = 0;
    timer_del(w->beacon_timer);
}

void esp_wifi_wmd_realize(EspWifiState *s, Error **errp)
{
    s->wmd.rx = g_byte_array_new();
    s->wmd.beacon_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wmd_beacon_timer,
                                       s);
    qemu_chr_fe_set_handlers(&s->wmd.chr, wmd_can_receive, wmd_receive,
                             wmd_event, NULL, s, NULL, true);
}
