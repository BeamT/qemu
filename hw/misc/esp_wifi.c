/*
 * Espressif Wi-Fi MAC emulation - chip independent core
 *
 * This file models the parts of the MAC that the IDF Wi-Fi driver talks to:
 * the TX slots, the RX descriptor chain, the interrupt cause registers and a
 * set of radio register blocks that only need to accept the PHY calibration
 * sequence. Frames are handed to esp_wifi_air.c, which emulates the rest of
 * the wireless medium.
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
#include "hw/irq.h"
#include "hw/qdev-properties.h"
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
#define TX_MAX_DESCRIPTORS  8

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
    esp_wifi_air_channel_changed(s);
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
    /* The frame is followed by an FCS, which the driver ignores */
    f = g_malloc0(sizeof(*f) + len + WLAN_FCS_LEN);
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

void esp_wifi_tx_complete(EspWifiState *s, int slot, bool acked)
{
    const EspWifiMacLayout *l = layout(s);

    if (!s->tx[slot].busy) {
        return;
    }
    s->tx[slot].busy = false;
    s->tx[slot].wait_ack = false;
    trace_esp_wifi_tx_done(slot, acked);

    /*
     * The IDF driver gets a TX done interrupt in both cases and tells them
     * apart by the PHY TX status. reverse-engineered
     */
    *mac_reg(s, l->phy_tx_status) = acked ? ESP_WIFI_PHY_TX_ACK
                                          : ESP_WIFI_PHY_TX_NO_ACK;
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
            esp_wifi_tx_complete(s, i, !s->tx[i].wait_ack);
        }
    }
    esp_wifi_tx_rearm(s);
}

static void esp_wifi_tx_start(EspWifiState *s, int slot, uint32_t plcp0)
{
    const EspWifiMacLayout *l = layout(s);
    uint8_t frame[ESP_WIFI_FRAME_MAX];
    uint32_t addr = dma_addr(s, plcp0);
    uint32_t plcp1 = l->plcp1_0 ?
                     *mac_reg(s, l->plcp1_0 + slot * l->plcp1_stride) : 0;
    size_t len = 0;
    int i;

    if (s->tx[slot].busy) {
        qemu_log_mask(LOG_GUEST_ERROR, "esp_wifi: TX slot %d already busy\n",
                      slot);
        return;
    }

    for (i = 0; i < TX_MAX_DESCRIPTORS && addr; i++) {
        uint32_t desc[3];
        uint32_t n;

        if (address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, desc, sizeof(desc))) {
            break;
        }
        desc[0] = le32_to_cpu(desc[0]);
        n = MIN(DESC_LENGTH(desc[0]), sizeof(frame) - len);
        address_space_read(&address_space_memory, le32_to_cpu(desc[1]),
                           MEMTXATTRS_UNSPECIFIED, frame + len, n);
        len += n;
        if ((desc[0] & DESC_EOF) || !desc[2]) {
            break;
        }
        addr = dma_addr(s, le32_to_cpu(desc[2]));
    }

    trace_esp_wifi_tx(slot, len, extract32(plcp1, 0, 12),
                      len >= 2 ? wlan_fc(frame) : 0);
    /* The length includes room for the FCS, which the MAC computes */
    len = len > WLAN_FCS_LEN ? len - WLAN_FCS_LEN : 0;
    s->tx[slot].busy = true;
    s->tx[slot].wait_ack = false;
    if (len >= 10 && esp_wifi_air_tx(s, slot, frame, len)) {
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
    esp_wifi_air_reset(s);
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
    esp_wifi_air_init(ESP_WIFI(obj));
}

static Property esp_wifi_properties[] = {
    DEFINE_NIC_PROPERTIES(EspWifiState, conf),
    DEFINE_PROP_STRING("ap0-ssid", EspWifiState, ap[0].ssid),
    DEFINE_PROP_STRING("ap0-psk", EspWifiState, ap[0].psk),
    DEFINE_PROP_UINT32("ap0-channel", EspWifiState, ap[0].channel, 1),
    DEFINE_PROP_INT32("ap0-rssi", EspWifiState, ap[0].rssi, -40),
    DEFINE_PROP_STRING("ap1-ssid", EspWifiState, ap[1].ssid),
    DEFINE_PROP_STRING("ap1-psk", EspWifiState, ap[1].psk),
    DEFINE_PROP_UINT32("ap1-channel", EspWifiState, ap[1].channel, 1),
    DEFINE_PROP_INT32("ap1-rssi", EspWifiState, ap[1].rssi, -40),
    DEFINE_PROP_STRING("ap2-ssid", EspWifiState, ap[2].ssid),
    DEFINE_PROP_STRING("ap2-psk", EspWifiState, ap[2].psk),
    DEFINE_PROP_UINT32("ap2-channel", EspWifiState, ap[2].channel, 1),
    DEFINE_PROP_INT32("ap2-rssi", EspWifiState, ap[2].rssi, -40),
    DEFINE_PROP_STRING("ap3-ssid", EspWifiState, ap[3].ssid),
    DEFINE_PROP_STRING("ap3-psk", EspWifiState, ap[3].psk),
    DEFINE_PROP_UINT32("ap3-channel", EspWifiState, ap[3].channel, 1),
    DEFINE_PROP_INT32("ap3-rssi", EspWifiState, ap[3].rssi, -40),
    DEFINE_PROP_STRING("client-psk", EspWifiState, client_psk),
    DEFINE_PROP_UINT32("ack-timeout-us", EspWifiState, ack_timeout_us, 20000),
    DEFINE_PROP_UINT32("beacon-interval", EspWifiState, beacon_interval_tu,
                       100),
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

    dc->realize = esp_wifi_realize;
    dc->desc = "Espressif Wi-Fi MAC";
    dc->vmsd = &vmstate_esp_wifi;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    device_class_set_props(dc, esp_wifi_properties);
    rc->phases.hold = esp_wifi_reset_hold;
}

static const TypeInfo esp_wifi_info = {
    .name = TYPE_ESP_WIFI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(EspWifiState),
    .instance_init = esp_wifi_init,
    .class_size = sizeof(EspWifiClass),
    .class_init = esp_wifi_class_init,
    .abstract = true,
};

static void esp_wifi_register_types(void)
{
    type_register_static(&esp_wifi_info);
}

type_init(esp_wifi_register_types)
