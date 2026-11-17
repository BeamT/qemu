/*
 * Espressif Wi-Fi MAC emulation - chip independent core
 *
 * This file models the parts of the MAC that the IDF Wi-Fi driver talks to:
 * the TX slots, the RX descriptor chain, the interrupt cause registers and a
 * set of radio register blocks that only need to accept the PHY calibration
 * sequence. Frames are handed to esp_wifi_air.c, which emulates the rest of
 * the wireless medium, or to esp_wifi_wmediumd.c, which puts them on the
 * medium simulated by wmediumd.
 *
 * Based on the ESP32 Wi-Fi emulation by Martin Johnson, its port to the
 * espressif/qemu tree by redfast00 and the ESP32-C3 port by
 * lcgamboa. See docs/wifi-provenance.md.
 *
 * Copyright (c) 2022 Martin Johnson
 * Copyright (c) 2023 redfast00
 * Copyright (c) 2023 lcgamboa
 * Copyright (c) 2026 Jason Kraus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "exec/address-spaces.h"
#include "sysemu/dma.h"
#include "hw/misc/esp_wifi.h"
#include "esp_wifi_int.h"
#include "trace.h"

/* DMA descriptor word 0. Same layout as the other Espressif DMA engines */
#define DESC_SIZE(w)        extract32(w, 0, 12)
#define DESC_LENGTH(w)      extract32(w, 12, 12)
#define DESC_EOF            BIT(30)
#define DESC_OWNER          BIT(31)
#define DESC_ADDR_MASK      0xfffff

#define TX_START            0xc0000000u
#define TX_LATENCY_NS       (20 * SCALE_US)
#define RX_RETRY_NS         (1 * SCALE_MS)
#define TX_MAX_DESCRIPTORS  ESP_WIFI_AMPDU_MAX

static inline EspWifiClass *esp_wifi_get_class(EspWifiState *s)
{
    return ESP_WIFI_GET_CLASS(s);
}

static inline const EspWifiMacLayout *layout(EspWifiState *s)
{
    return esp_wifi_get_class(s)->layout;
}

static inline uint32_t *mac_reg(EspWifiState *s, hwaddr addr)
{
    return &s->mac_regs[addr / 4];
}

static void esp_wifi_update_irq(EspWifiState *s)
{
    const EspWifiMacLayout *l = layout(s);

    *mac_reg(s, l->int_status) = s->raw_int;
    qemu_set_irq(s->irq, s->raw_int != 0);
}

static void esp_wifi_raise(EspWifiState *s, uint32_t cause)
{
    s->raw_int |= cause;
    esp_wifi_update_irq(s);
}

uint64_t esp_wifi_tsf(EspWifiState *s)
{
    return qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
}

const uint8_t *esp_wifi_own_mac(EspWifiState *s, int iface)
{
    const EspWifiMacLayout *l = layout(s);
    uint32_t lo = *mac_reg(s, l->addr_filter[iface]);
    uint32_t hi = *mac_reg(s, l->addr_filter[iface] + 4);
    uint8_t *mac = s->own_mac[iface];

    stl_le_p(mac, lo);
    stw_le_p(mac + 4, hi);
    return mac;
}

void esp_wifi_set_channel(EspWifiState *s, uint32_t channel)
{
    if (channel == s->channel) {
        return;
    }
    trace_esp_wifi_channel(s->channel, channel);
    s->channel = channel;
    if (!esp_wifi_wmd_enabled(s)) {
        esp_wifi_air_channel_changed(s);
    }
}

/* RX path: hand frames to the guest through its descriptor chain */

static uint32_t dma_addr(EspWifiState *s, uint32_t low)
{
    return layout(s)->dma_base | (low & DESC_ADDR_MASK);
}

static bool esp_wifi_rx_one(EspWifiState *s, EspWifiFrame *f)
{
    EspWifiClass *k = esp_wifi_get_class(s);
    const EspWifiMacLayout *l = layout(s);
    uint32_t desc[3];
    size_t total = k->rx_ctrl_size + f->len;
    g_autofree uint8_t *buf = NULL;

    if (!s->rx_cur) {
        return false;
    }
    if (address_space_read(&address_space_memory, s->rx_cur,
                           MEMTXATTRS_UNSPECIFIED, desc, sizeof(desc))) {
        qemu_log_mask(LOG_GUEST_ERROR, "esp_wifi: bad RX descriptor 0x%x\n",
                      s->rx_cur);
        s->rx_cur = 0;
        return false;
    }
    desc[0] = le32_to_cpu(desc[0]);
    desc[1] = le32_to_cpu(desc[1]);
    desc[2] = le32_to_cpu(desc[2]);

    /*
     * Only fill a descriptor that the driver handed over and has not been
     * filled yet; anywhere else would corrupt the guest heap.
     */
    if (!(desc[0] & DESC_OWNER) || (desc[0] & DESC_EOF) || desc[1] == 0) {
        trace_esp_wifi_rx_stall(s->rx_cur, desc[0]);
        return false;
    }
    if (DESC_SIZE(desc[0]) < total) {
        /* The hardware drops frames that do not fit */
        trace_esp_wifi_rx_drop(f->len, DESC_SIZE(desc[0]));
        return true;
    }

    buf = g_malloc0(total);
    k->fill_rx_ctrl(s, buf, f);
    memcpy(buf + k->rx_ctrl_size, f->data, f->len);
    address_space_write(&address_space_memory, desc[1],
                        MEMTXATTRS_UNSPECIFIED, buf, total);

    desc[0] = deposit32(desc[0], 12, 12, total) | DESC_EOF;
    desc[0] = cpu_to_le32(desc[0]);
    address_space_write(&address_space_memory, s->rx_cur,
                        MEMTXATTRS_UNSPECIFIED, desc, 4);

    trace_esp_wifi_rx(s->rx_cur, f->len, wlan_fc(f->data));
    *mac_reg(s, l->rx_last) = s->rx_cur;
    s->rx_cur = desc[2] ? dma_addr(s, desc[2]) : 0;
    esp_wifi_raise(s, l->int_rx);
    return true;
}

static void esp_wifi_rx_flush(EspWifiState *s)
{
    EspWifiFrame *f;
    bool was_full = s->rx_queue_len >= ESP_WIFI_RX_QUEUE_MAX;

    while ((f = QSIMPLEQ_FIRST(&s->rx_queue))) {
        if (!esp_wifi_rx_one(s, f)) {
            break;
        }
        QSIMPLEQ_REMOVE_HEAD(&s->rx_queue, next);
        s->rx_queue_len--;
        g_free(f);
    }
    if (s->rx_queue_len) {
        timer_mod(s->rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RX_RETRY_NS);
    }
    if (was_full && s->rx_queue_len < ESP_WIFI_RX_QUEUE_MAX && s->nic) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void esp_wifi_rx_timer(void *opaque)
{
    esp_wifi_rx_flush(opaque);
}

void esp_wifi_deliver(EspWifiState *s, const uint8_t *frame, size_t len,
                      int8_t rssi)
{
    EspWifiFrame *f;

    if (len + WLAN_FCS_LEN > ESP_WIFI_FRAME_MAX) {
        return;
    }
    /* Nothing is received until the driver has set up its RX buffers */
    if (!*mac_reg(s, layout(s)->rx_base)) {
        return;
    }
    if (s->rx_queue_len >= ESP_WIFI_RX_QUEUE_MAX) {
        trace_esp_wifi_rx_drop(len, 0);
        return;
    }
    /*
     * The frame is followed by an FCS, which the driver ignores. Short
     * control frames get room for a whole header, which the RX control
     * header code looks into.
     */
    f = g_malloc0(sizeof(*f) + MAX(len, WLAN_HDR_LEN) + WLAN_FCS_LEN);
    f->rssi = rssi;
    f->len = len + WLAN_FCS_LEN;
    memcpy(f->data, frame, len);
    QSIMPLEQ_INSERT_TAIL(&s->rx_queue, f, next);
    s->rx_queue_len++;
    esp_wifi_rx_flush(s);
}

static void esp_wifi_rx_purge(EspWifiState *s)
{
    EspWifiFrame *f;

    while ((f = QSIMPLEQ_FIRST(&s->rx_queue))) {
        QSIMPLEQ_REMOVE_HEAD(&s->rx_queue, next);
        g_free(f);
    }
    s->rx_queue_len = 0;
}

/* RX descriptor reload: continue after the last filled descriptor */
static void esp_wifi_rx_reload(EspWifiState *s)
{
    const EspWifiMacLayout *l = layout(s);
    uint32_t last = *mac_reg(s, l->rx_last);
    uint32_t next;

    if (!s->rx_cur && last) {
        next = address_space_ldl_le(&address_space_memory, last + 8,
                                    MEMTXATTRS_UNSPECIFIED, NULL);
        if (next) {
            s->rx_cur = dma_addr(s, next);
        }
    }
    esp_wifi_rx_flush(s);
}

/* TX path */

/*
 * The block ack of an A-MPDU, where the driver reads it after a successful
 * transmission (hal_mac_tx_get_blockack): TID and starting sequence number,
 * and a 64 bit bitmap with bit n set when sequence number SSN + n arrived.
 */
static void esp_wifi_tx_block_ack(EspWifiState *s, int slot)
{
    const EspWifiMacLayout *l = layout(s);
    EspWifiTxSlot *t = &s->tx[slot];
    hwaddr off = slot * l->phy_tx_status_stride;
    uint16_t ssn = t->mpdu_seq[0];
    uint64_t bitmap = 0;
    unsigned i;

    for (i = 0; i < t->n_ampdu; i++) {
        unsigned n = (t->mpdu_seq[i] - ssn) & 0xfff;

        if ((t->ba & BIT(i)) && n < 64) {
            bitmap |= BIT_ULL(n);
        }
    }
    *mac_reg(s, l->ba_info + off) = deposit32(ssn, 12, 4, t->ampdu_tid);
    *mac_reg(s, l->ba_bitmap + off) = bitmap;
    *mac_reg(s, l->ba_bitmap - 4 + off) = bitmap >> 32;
}

void esp_wifi_tx_complete(EspWifiState *s, int slot, bool acked)
{
    const EspWifiMacLayout *l = layout(s);
    uint32_t status;

    if (!s->tx[slot].busy) {
        return;
    }
    s->tx[slot].busy = false;
    s->tx[slot].wait_ack = false;
    trace_esp_wifi_tx_done(slot, acked);

    /*
     * The IDF driver gets a TX done interrupt in both cases and tells them
     * apart by the PHY TX status, a copy per slot. Bits 15:12 are the
     * result: 0 success, 2 ACK timeout (lmacProcessTxComplete).
     */
    status = acked ? ESP_WIFI_PHY_TX_ACK : ESP_WIFI_PHY_TX_NO_ACK;
    if (s->tx[slot].n_ampdu && l->ba_info) {
        /* A block ack came back if any MPDU arrived */
        esp_wifi_tx_block_ack(s, slot);
        status = s->tx[slot].ba ? ESP_WIFI_PHY_TX_ACK : ESP_WIFI_PHY_TX_NO_ACK;
    }
    if (l->phy_tx_status_stride) {
        *mac_reg(s, l->phy_tx_status + slot * l->phy_tx_status_stride) =
            status;
    }
    if (!slot || !l->phy_tx_status_stride) {
        *mac_reg(s, l->phy_tx_status) = status;
    }
    *mac_reg(s, l->txq_complete) |= BIT(slot);
    esp_wifi_raise(s, ESP_WIFI_INT_TX_DONE);
}

static void esp_wifi_tx_rearm(EspWifiState *s)
{
    int64_t next = INT64_MAX;
    int i;

    for (i = 0; i < ESP_WIFI_TX_SLOTS; i++) {
        if (s->tx[i].busy) {
            next = MIN(next, s->tx[i].deadline);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->ack_timer);
    } else {
        timer_mod(s->ack_timer, next);
    }
}

/* Complete transmissions that need no ACK, and time out the others */
static void esp_wifi_ack_timer(void *opaque)
{
    EspWifiState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int i;

    for (i = 0; i < ESP_WIFI_TX_SLOTS; i++) {
        if (s->tx[i].busy && s->tx[i].deadline <= now) {
            bool acked = !s->tx[i].wait_ack && s->tx[i].ok;

            if (!acked) {
                s->tx[i].ba = 0;
            }
            esp_wifi_tx_complete(s, i, acked);
        }
    }
    esp_wifi_tx_rearm(s);
}

/*
 * PLCP0 bits 25:24: 0 for frames that expect no ACK, 1 for frames that
 * expect one, 2 for an A-MPDU. The descriptors of an A-MPDU hold one MPDU
 * each, after an 8 byte subframe header whose first 16 bits are the MPDU
 * length including the FCS. reverse-engineered
 */
#define TX_ACK_BLOCK        2
#define AMPDU_HDR_LEN       8

static void esp_wifi_tx_start(EspWifiState *s, int slot, uint32_t plcp0)
{
    const EspWifiMacLayout *l = layout(s);
    uint8_t *frame = s->tx_buf;
    uint32_t addr = dma_addr(s, plcp0);
    uint32_t plcp1 = l->plcp1_0 ?
                     *mac_reg(s, l->plcp1_0 + slot * l->plcp1_stride) : 0;
    unsigned ack_mode = extract32(plcp0, 24, 2);
    size_t off[TX_MAX_DESCRIPTORS], dlen[TX_MAX_DESCRIPTORS];
    size_t mpdu_off[TX_MAX_DESCRIPTORS], mpdu_len[TX_MAX_DESCRIPTORS];
    unsigned n_desc = 0, n_mpdu, i;
    bool ampdu = ack_mode == TX_ACK_BLOCK, wait = false;
    size_t len = 0;

    if (s->tx[slot].busy) {
        qemu_log_mask(LOG_GUEST_ERROR, "esp_wifi: TX slot %d already busy\n",
                      slot);
        return;
    }

    while (n_desc < TX_MAX_DESCRIPTORS && addr) {
        uint32_t desc[3];
        uint32_t n;

        if (address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, desc, sizeof(desc))) {
            break;
        }
        desc[0] = le32_to_cpu(desc[0]);
        n = MIN(DESC_LENGTH(desc[0]), ESP_WIFI_TX_BUF - len);
        address_space_read(&address_space_memory, le32_to_cpu(desc[1]),
                           MEMTXATTRS_UNSPECIFIED, frame + len, n);
        off[n_desc] = len;
        dlen[n_desc++] = n;
        len += n;
        if ((desc[0] & DESC_EOF) || !desc[2]) {
            break;
        }
        addr = dma_addr(s, le32_to_cpu(desc[2]));
    }

    /* Each MPDU of an A-MPDU, or the whole frame */
    for (i = 0; ampdu && i < n_desc; i++) {
        size_t m = dlen[i] >= AMPDU_HDR_LEN ?
                   lduw_le_p(frame + off[i]) : 0;

        if (m < WLAN_FCS_LEN || m + AMPDU_HDR_LEN > dlen[i]) {
            ampdu = false;
        }
        mpdu_off[i] = off[i] + AMPDU_HDR_LEN;
        mpdu_len[i] = m;
    }
    if (ampdu) {
        n_mpdu = n_desc;
    } else {
        n_mpdu = 1;
        mpdu_off[0] = 0;
        mpdu_len[0] = len;
    }

    trace_esp_wifi_tx(slot, len, extract32(plcp1, 0, 12),
                      mpdu_len[0] >= 2 ? wlan_fc(frame + mpdu_off[0]) : 0);
    s->tx[slot].busy = true;
    s->tx[slot].wait_ack = false;
    s->tx[slot].ok = true;
    s->tx[slot].pending = 0;
    s->tx[slot].n_ampdu = ampdu ? n_mpdu : 0;
    /* Media other than wmediumd acknowledge all or nothing */
    s->tx[slot].ba = MAKE_64BIT_MASK(0, n_mpdu);
    for (i = 0; i < n_mpdu; i++) {
        /* The length includes room for the FCS, which the MAC computes */
        size_t n = mpdu_len[i] > WLAN_FCS_LEN ? mpdu_len[i] - WLAN_FCS_LEN
                                              : 0;

        if (n < 10) {
            continue;
        }
        if (n > ESP_WIFI_FRAME_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR, "esp_wifi: %zu byte frame too "
                          "long, dropped\n", n);
            s->tx[slot].ok = false;
            s->tx[slot].ba &= ~BIT(i);
            continue;
        }
        if (ampdu && n >= WLAN_HDR_LEN + 2) {
            s->tx[slot].mpdu_seq[i] = lduw_le_p(frame + mpdu_off[i] + 22) >> 4;
            s->tx[slot].ampdu_tid = frame[mpdu_off[i] + WLAN_HDR_LEN] & 0xf;
        }
        if (esp_wifi_wmd_enabled(s)) {
            esp_wifi_wmd_tx(s, slot, frame + mpdu_off[i], n);
        } else {
            wait |= esp_wifi_air_tx(s, slot, frame + mpdu_off[i], n);
        }
    }
    if (s->tx[slot].pending) {
        /* Wait for wmediumd's TX status, but not forever */
        s->tx[slot].wait_ack = true;
        s->tx[slot].deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                               (int64_t)s->air_timeout_ms * SCALE_MS;
    } else if (wait) {
        s->tx[slot].wait_ack = true;
        s->tx[slot].deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                               (int64_t)s->ack_timeout_us * SCALE_US;
    } else {
        s->tx[slot].deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                               TX_LATENCY_NS;
    }
    esp_wifi_tx_rearm(s);
}

static int esp_wifi_tx_slot(EspWifiState *s, hwaddr addr)
{
    const EspWifiMacLayout *l = layout(s);
    int i;

    for (i = 0; i < ESP_WIFI_TX_SLOTS; i++) {
        if (addr == l->tx_plcp0 + i * l->tx_stride) {
            return i;
        }
    }
    return -1;
}

/* MAC register block */

static void esp_wifi_check_known(EspWifiState *s, hwaddr addr, bool write)
{
    EspWifiClass *k = esp_wifi_get_class(s);

    if (k->mac_reg_known(addr) || esp_wifi_tx_slot(s, addr) >= 0) {
        return;
    }
    if (!test_and_set_bit(addr / 4, s->unimp_seen)) {
        qemu_log_mask(LOG_UNIMP, "esp_wifi: unmodelled MAC register 0x%03"
                      HWADDR_PRIx " %s, stored as plain memory\n", addr,
                      write ? "written" : "read");
    }
}

static uint64_t esp_wifi_mac_read(void *opaque, hwaddr addr, unsigned size)
{
    EspWifiState *s = opaque;
    const EspWifiMacLayout *l = layout(s);
    uint32_t r = *mac_reg(s, addr);
    int i;

    for (i = 0; i < ARRAY_SIZE(l->status) && l->status[i].addr; i++) {
        if (addr == l->status[i].addr) {
            r = (r & ~l->status[i].clear) | l->status[i].set;
        }
    }
    if (addr == l->rx_next) {
        r = s->rx_cur ? s->rx_cur : l->dma_base;
    } else if (addr == l->int_status || addr == l->int_clear) {
        r = s->raw_int;
    } else if (l->mac_ctrl && addr == l->mac_ctrl) {
        r &= ~l->mac_ctrl_busy;
    }
    esp_wifi_check_known(s, addr, false);
    trace_esp_wifi_mac_read(addr, r);
    return r;
}

static void esp_wifi_mac_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    EspWifiState *s = opaque;
    const EspWifiMacLayout *l = layout(s);
    int slot;

    trace_esp_wifi_mac_write(addr, value);
    esp_wifi_check_known(s, addr, true);
    *mac_reg(s, addr) = value;
    esp_wifi_keys_written(s, addr);

    if (addr == l->rx_base) {
        s->rx_cur = value ? dma_addr(s, value) : 0;
        esp_wifi_rx_flush(s);
    } else if (addr == l->rx_ctrl) {
        if (value & 1) {
            esp_wifi_rx_reload(s);
        }
    } else if (addr == l->int_clear) {
        s->raw_int &= ~value;
        esp_wifi_update_irq(s);
    } else if (addr == l->txq_complete_clear) {
        *mac_reg(s, l->txq_complete) &= ~value;
    } else if (addr == l->txq_error_clear) {
        *mac_reg(s, l->txq_error) &= ~value;
    } else if (value & TX_START) {
        slot = esp_wifi_tx_slot(s, addr);
        if (slot >= 0) {
            esp_wifi_tx_start(s, slot, value);
        }
    }
}

static const MemoryRegionOps esp_wifi_mac_ops = {
    .read = esp_wifi_mac_read,
    .write = esp_wifi_mac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Radio register blocks */

static uint64_t esp_wifi_radio_read(void *opaque, hwaddr addr, unsigned size)
{
    EspWifiRadioRegion *r = opaque;
    EspWifiClass *k = esp_wifi_get_class(r->s);
    uint64_t v = r->desc->constant ? r->desc->read_value : r->regs[addr / 4];

    if (k->radio_read) {
        v = k->radio_read(r->s, r->desc, addr, v);
    }
    trace_esp_wifi_radio_read(r->desc->name, addr, v);
    return v;
}

static void esp_wifi_radio_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    EspWifiRadioRegion *r = opaque;
    EspWifiClass *k = esp_wifi_get_class(r->s);

    trace_esp_wifi_radio_write(r->desc->name, addr, value);
    r->regs[addr / 4] = value;
    if (k->radio_write) {
        k->radio_write(r->s, r->desc, addr, value);
    }
}

static const MemoryRegionOps esp_wifi_radio_ops = {
    .read = esp_wifi_radio_read,
    .write = esp_wifi_radio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void esp_wifi_map(EspWifiState *s,
                  void (*map)(void *opaque, hwaddr base, MemoryRegion *mr),
                  void *opaque)
{
    EspWifiClass *k = esp_wifi_get_class(s);
    unsigned i;

    map(opaque, k->radio[0].base, &s->mac_iomem);
    for (i = 1; i < k->n_radio; i++) {
        map(opaque, k->radio[i].base, &s->radio[i].iomem);
    }
}

/* NIC backend */

static bool esp_wifi_can_receive(NetClientState *nc)
{
    EspWifiState *s = qemu_get_nic_opaque(nc);

    return s->rx_queue_len < ESP_WIFI_RX_QUEUE_MAX;
}

static ssize_t esp_wifi_receive(NetClientState *nc, const uint8_t *buf,
                                size_t size)
{
    EspWifiState *s = qemu_get_nic_opaque(nc);

    esp_wifi_air_rx_eth(s, buf, size);
    return size;
}

void esp_wifi_send_eth(EspWifiState *s, const uint8_t *buf, size_t len)
{
    qemu_send_packet(qemu_get_queue(s->nic), buf, len);
}

static NetClientInfo esp_wifi_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = esp_wifi_can_receive,
    .receive = esp_wifi_receive,
};

/* Device */

static void esp_wifi_reset_hold(Object *obj, ResetType type)
{
    EspWifiState *s = ESP_WIFI(obj);
    EspWifiClass *k = esp_wifi_get_class(s);
    unsigned i;

    trace_esp_wifi_reset();
    memset(s->mac_regs, 0, layout(s)->size);
    for (i = 1; i < k->n_radio; i++) {
        uint32_t n = k->radio[i].size / 4;
        uint32_t j;

        for (j = 0; j < n; j++) {
            s->radio[i].regs[j] = k->radio[i].read_value;
        }
    }
    s->raw_int = 0;
    s->rx_cur = 0;
    s->channel = 0;
    s->seq = 0;
    memset(s->tx, 0, sizeof(s->tx));
    esp_wifi_rx_purge(s);
    timer_del(s->rx_timer);
    timer_del(s->ack_timer);
    esp_wifi_keys_reset(s);
    if (esp_wifi_wmd_enabled(s)) {
        esp_wifi_wmd_reset(s);
    } else {
        esp_wifi_air_reset(s);
    }
    qemu_set_irq(s->irq, 0);
}

static void esp_wifi_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    EspWifiState *s = ESP_WIFI(dev);
    EspWifiClass *k = esp_wifi_get_class(s);
    const EspWifiMacLayout *l = k->layout;
    unsigned i;

    assert(k->n_radio <= ESP_WIFI_RADIO_MAX);
    s->mac_regs = g_malloc0(l->size);
    s->tx_buf = g_malloc(ESP_WIFI_TX_BUF);
    s->unimp_seen = bitmap_new(l->size / 4);
    memory_region_init_io(&s->mac_iomem, OBJECT(dev), &esp_wifi_mac_ops, s,
                          "esp-wifi.mac", l->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mac_iomem);

    /* Entry 0 of the radio table describes the MAC block itself */
    for (i = 1; i < k->n_radio; i++) {
        EspWifiRadioRegion *r = &s->radio[i];
        g_autofree char *name = g_strdup_printf("esp-wifi.%s",
                                                k->radio[i].name);

        r->s = s;
        r->desc = &k->radio[i];
        r->regs = g_malloc0(k->radio[i].size);
        memory_region_init_io(&r->iomem, OBJECT(dev), &esp_wifi_radio_ops, r,
                              name, k->radio[i].size);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &r->iomem);
    }
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    QSIMPLEQ_INIT(&s->rx_queue);
    s->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, esp_wifi_rx_timer, s);
    s->ack_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, esp_wifi_ack_timer, s);
    if (esp_wifi_wmd_enabled(s)) {
        /* No emulated peers, and no netdev: everything goes to wmediumd */
        esp_wifi_wmd_realize(s, errp);
        return;
    }
    esp_wifi_air_realize(s, errp);
    if (*errp) {
        return;
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&esp_wifi_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void esp_wifi_init(Object *obj)
{
    EspWifiState *s = ESP_WIFI(obj);
    int i;

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        s->ap[i].channel = 1;
        s->ap[i].rssi = -40;
        s->ap[i].uplink = true;
        s->ap[i].dhcp = true;
    }
    esp_wifi_air_init(s);
}

static void esp_wifi_finalize(Object *obj)
{
    EspWifiState *s = ESP_WIFI(obj);
    int i;

    esp_wifi_keys_free(s);

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        g_free(s->ap[i].ssid);
        g_free(s->ap[i].psk);
    }
}

/*
 * The emulated access points are QOM properties rather than qdev ones so
 * that they can be changed while the guest runs (qom-set), e.g. to take a
 * network off the air or change its passphrase.
 */
enum { AP_SSID, AP_PSK, AP_CHANNEL, AP_RSSI, AP_SILENT, AP_UPLINK, AP_DHCP };

static const struct {
    const char *name, *type, *desc;
} ap_props[] = {
    [AP_SSID] = { "ssid", "str", "SSID of the access point, empty: none" },
    [AP_PSK] = { "psk", "str", "WPA2 passphrase, empty: open network" },
    [AP_CHANNEL] = { "channel", "uint32", "Channel, 1 to 14" },
    [AP_RSSI] = { "rssi", "int32", "Signal strength at the station, dBm" },
    [AP_SILENT] = { "silent", "bool",
                    "Beacon and answer probes but ignore authentication" },
    [AP_UPLINK] = { "uplink", "bool",
                    "Forward station traffic other than ARP and DHCP" },
    [AP_DHCP] = { "dhcp", "bool",
                  "Forward the station's DHCP requests to the DHCP server" },
};

#define AP_OPAQUE(i, p) ((void *)(uintptr_t)((i) << 8 | (p)))
#define AP_INDEX(o)     ((uintptr_t)(o) >> 8)
#define AP_PROP(o)      ((uintptr_t)(o) & 0xff)

static void esp_wifi_get_ap(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    EspWifiAp *ap = &ESP_WIFI(obj)->ap[AP_INDEX(opaque)];
    g_autofree char *str = NULL;

    switch (AP_PROP(opaque)) {
    case AP_SSID:
    case AP_PSK:
        str = g_strdup(AP_PROP(opaque) == AP_SSID ? ap->ssid : ap->psk);
        if (!str) {
            str = g_strdup("");
        }
        visit_type_str(v, name, &str, errp);
        break;
    case AP_CHANNEL:
        visit_type_uint32(v, name, &ap->channel, errp);
        break;
    case AP_RSSI:
        visit_type_int32(v, name, &ap->rssi, errp);
        break;
    case AP_SILENT:
        visit_type_bool(v, name, &ap->silent, errp);
        break;
    case AP_UPLINK:
        visit_type_bool(v, name, &ap->uplink, errp);
        break;
    case AP_DHCP:
        visit_type_bool(v, name, &ap->dhcp, errp);
        break;
    }
}

static void esp_wifi_set_ap(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    EspWifiState *s = ESP_WIFI(obj);
    int i = AP_INDEX(opaque);
    EspWifiAp *ap = &s->ap[i];
    char *str, **field;
    uint32_t channel;

    switch (AP_PROP(opaque)) {
    case AP_SSID:
    case AP_PSK:
        if (!visit_type_str(v, name, &str, errp)) {
            return;
        }
        if (AP_PROP(opaque) == AP_SSID && strlen(str) > 32) {
            error_setg(errp, "%s must be at most 32 characters", name);
            g_free(str);
            return;
        }
        if (AP_PROP(opaque) == AP_PSK && *str &&
            (strlen(str) < 8 || strlen(str) > 63)) {
            error_setg(errp, "%s must be 8 to 63 characters", name);
            g_free(str);
            return;
        }
        field = AP_PROP(opaque) == AP_SSID ? &ap->ssid : &ap->psk;
        if (!g_strcmp0(*field ? *field : "", str)) {
            g_free(str);
            return;
        }
        g_free(*field);
        *field = str;
        break;
    case AP_CHANNEL:
        if (!visit_type_uint32(v, name, &channel, errp)) {
            return;
        }
        if (channel < 1 || channel > 14) {
            error_setg(errp, "%s must be between 1 and 14", name);
            return;
        }
        if (channel == ap->channel) {
            return;
        }
        ap->channel = channel;
        break;
    /* These take effect on the next frame without dropping the link */
    case AP_RSSI:
        visit_type_int32(v, name, &ap->rssi, errp);
        return;
    case AP_SILENT:
        visit_type_bool(v, name, &ap->silent, errp);
        return;
    case AP_UPLINK:
        visit_type_bool(v, name, &ap->uplink, errp);
        return;
    case AP_DHCP:
        visit_type_bool(v, name, &ap->dhcp, errp);
        return;
    }
    if (DEVICE(obj)->realized && !esp_wifi_wmd_enabled(s)) {
        esp_wifi_air_ap_changed(s, i, errp);
    }
}

static Property esp_wifi_properties[] = {
    DEFINE_NIC_PROPERTIES(EspWifiState, conf),
    DEFINE_PROP_STRING("client-psk", EspWifiState, client_psk),
    DEFINE_PROP_UINT32("ack-timeout-us", EspWifiState, ack_timeout_us, 20000),
    DEFINE_PROP_UINT32("beacon-interval", EspWifiState, beacon_interval_tu,
                       100),
    DEFINE_PROP_CHR("air", EspWifiState, wmd.chr),
    DEFINE_PROP_UINT32("air-timeout-ms", EspWifiState, air_timeout_ms, 1000),
    DEFINE_PROP_END_OF_LIST(),
};

/* The frame queues, timers and emulated peers are not migrated */
static const VMStateDescription vmstate_esp_wifi = {
    .name = "esp-wifi",
    .unmigratable = 1,
};

static void esp_wifi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    int i, p;

    dc->realize = esp_wifi_realize;
    dc->desc = "Espressif Wi-Fi MAC";
    dc->vmsd = &vmstate_esp_wifi;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    device_class_set_props(dc, esp_wifi_properties);
    rc->phases.hold = esp_wifi_reset_hold;

    for (i = 0; i < ESP_WIFI_MAX_APS; i++) {
        for (p = 0; p < ARRAY_SIZE(ap_props); p++) {
            g_autofree char *name = g_strdup_printf("ap%d-%s", i,
                                                    ap_props[p].name);

            object_class_property_add(klass, name, ap_props[p].type,
                                      esp_wifi_get_ap, esp_wifi_set_ap, NULL,
                                      AP_OPAQUE(i, p));
            object_class_property_set_description(klass, name,
                                                  ap_props[p].desc);
        }
    }
}

DeviceState *esp_wifi_create(const char *type)
{
    DeviceState *dev = qemu_create_nic_device(type, false, NULL);

    if (dev) {
        return dev;
    }
    /*
     * No -nic for this model. The device still exists if -global gave it a
     * chardev for wmediumd, so that it can be used with -nic none.
     */
    dev = qdev_new(type);
    if (!esp_wifi_wmd_enabled(ESP_WIFI(dev))) {
        object_unref(OBJECT(dev));
        return NULL;
    }
    return dev;
}

static const TypeInfo esp_wifi_info = {
    .name = TYPE_ESP_WIFI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(EspWifiState),
    .instance_init = esp_wifi_init,
    .instance_finalize = esp_wifi_finalize,
    .class_size = sizeof(EspWifiClass),
    .class_init = esp_wifi_class_init,
    .abstract = true,
};

static void esp_wifi_register_types(void)
{
    type_register_static(&esp_wifi_info);
}

type_init(esp_wifi_register_types)
