/*
 * Espressif Wi-Fi MAC emulation - chip independent core
 *
 * The Wi-Fi MAC and radio of Espressif chips are undocumented. Every register
 * offset and bit used by this model was found by observing the IDF Wi-Fi
 * driver at run time or taken from the esp32-open-mac project; see
 * docs/wifi.md for the register map and the confidence level of each entry.
 *
 * Copyright (c) 2022 Martin Johnson
 * Copyright (c) 2023 redfast00
 * Copyright (c) 2023 lcgamboa
 * Copyright (c) 2026 Jason Kraus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "hw/sysbus.h"
#include "net/net.h"
#include "chardev/char-fe.h"
#include "qemu/timer.h"
#include "qemu/queue.h"
#include "qom/object.h"

#define TYPE_ESP_WIFI "misc.esp_wifi"
OBJECT_DECLARE_TYPE(EspWifiState, EspWifiClass, ESP_WIFI)

#define ESP_WIFI_TX_SLOTS       5
#define ESP_WIFI_MAX_APS        4
#define ESP_WIFI_MAX_CLIENTS    4
#define ESP_WIFI_RADIO_MAX      10
#define ESP_WIFI_RX_QUEUE_MAX   64
#define ESP_WIFI_FRAME_MAX      2400
/* MPDUs in an A-MPDU (the IDF TX block ack window is at most 32) */
#define ESP_WIFI_AMPDU_MAX      32
#define ESP_WIFI_TX_BUF         (ESP_WIFI_AMPDU_MAX * ESP_WIFI_FRAME_MAX)
#define ESP_WIFI_KEYS           32
#define ESP_WIFI_KEY_ENTRY_SIZE 0x28
/* Replay counters per key: one per TID, one for management frames */
#define ESP_WIFI_KEY_RX_PN      17

/* TX done interrupt cause, shared by all supported chips. reverse-engineered */
#define ESP_WIFI_INT_TX_DONE        0x00000080

/* Values of the PHY TX status register. reverse-engineered */
#define ESP_WIFI_PHY_TX_ACK         0x00000000
#define ESP_WIFI_PHY_TX_NO_ACK      0x00002300

/*
 * Offsets of the MAC registers the model acts on, relative to the start of
 * the MAC region. A zero offset means the chip has no such register (or it is
 * not known yet).
 */
/* Status bits that read as done: commands complete as soon as issued */
typedef struct EspWifiStatusBits {
    hwaddr addr;
    uint32_t clear;             /* command bits the hardware clears */
    uint32_t set;               /* idle/ready bits */
} EspWifiStatusBits;

typedef struct EspWifiMacLayout {
    hwaddr size;
    hwaddr addr_filter[2];      /* own MAC address, interface 0/1 */
    hwaddr rx_ctrl;             /* bit 0: reload RX descriptors */
    hwaddr rx_base;             /* first RX descriptor */
    hwaddr rx_next;             /* descriptor the next frame goes into */
    hwaddr rx_last;             /* last descriptor filled */
    hwaddr int_status;
    hwaddr int_clear;
    hwaddr mac_ctrl;            /* optional */
    hwaddr txq_error_clear;
    hwaddr txq_error;           /* [15:0] collision, [31:16] timeout */
    hwaddr txq_complete_clear;
    hwaddr txq_complete;
    hwaddr tx_config0;          /* slot n at tx_config0 + n * tx_stride */
    hwaddr tx_plcp0;            /* slot 0, bits [31:30] start transmission */
    int tx_stride;
    hwaddr plcp1_0;             /* optional; slot 0, [11:0] frame length */
    int plcp1_stride;
    hwaddr phy_tx_status;       /* 0x2300 when no ACK was received */
    int phy_tx_status_stride;   /* optional; per slot copies */
    hwaddr ba_info;             /* optional; slot 0 block ack TID and SSN */
    hwaddr ba_bitmap;           /* slot 0 bitmap bits 31:0, 63:32 below */
    uint32_t mac_ctrl_busy;     /* bits that read back 0 once the MAC stops */
    uint32_t int_rx;            /* cause raised when a frame is received */
    hwaddr dma_base;            /* descriptors hold the low 20 address bits */
    hwaddr key_table;           /* optional; ESP_WIFI_KEYS entries of 0x28 */
    hwaddr key_valid;           /* bit n: key entry n in use */
    EspWifiStatusBits status[4];
} EspWifiMacLayout;

/*
 * A block of radio (PHY, RF front end, baseband...) registers that the Wi-Fi
 * driver initialises and polls. Nothing in these blocks is modelled beyond
 * storing writes and returning values that let the calibration code finish.
 */
typedef struct EspWifiRadioBlock {
    const char *name;
    hwaddr base;
    hwaddr size;
    bool constant;              /* reads always return read_value */
    uint32_t read_value;        /* reset value, or the constant */
} EspWifiRadioBlock;

typedef struct EspWifiRadioRegion {
    MemoryRegion iomem;
    EspWifiState *s;
    const EspWifiRadioBlock *desc;
    uint32_t *regs;
} EspWifiRadioRegion;

typedef struct EspWifiFrame {
    QSIMPLEQ_ENTRY(EspWifiFrame) next;
    int8_t rssi;
    uint32_t len;
    uint8_t data[];
} EspWifiFrame;

typedef enum EspWifiLinkState {
    ESP_WIFI_LINK_NONE,
    ESP_WIFI_LINK_AUTHENTICATED,
    ESP_WIFI_LINK_HANDSHAKE,    /* waiting for the WPA2 4-way handshake */
    ESP_WIFI_LINK_ESTABLISHED,
} EspWifiLinkState;

/* State of a WPA2-PSK 4-way handshake, from either side. */
typedef struct EspWifiRsnKeys {
    uint8_t anonce[32];
    uint8_t snonce[32];
    uint8_t ptk[48];            /* KCK | KEK | TK */
    uint64_t replay;
    uint64_t tx_pn;
    uint8_t akm;                /* 00-0f-ac suite: 2 PSK, 6 PSK-SHA256 */
    bool ptk_valid;
} EspWifiRsnKeys;

/* An access point emulated by the model, which the guest can join. */
typedef struct EspWifiAp {
    char *ssid;
    char *psk;                  /* empty: open network */
    uint32_t channel;
    int32_t rssi;
    bool silent;                /* beacons, but ignores authentication */
    bool uplink;                /* false: only ARP and DHCP get through */
    bool dhcp;                  /* false: the DHCP server does not answer */
    MACAddr bssid;
    MACAddr sta;                /* the guest station using this AP */
    uint8_t pmk[32];
    EspWifiLinkState link;
    EspWifiRsnKeys keys;
    int eapol_msg;              /* EAPOL-Key message to (re)send, 1 or 3 */
    int eapol_tries;
    int64_t eapol_due;
} EspWifiAp;

/*
 * A station emulated by the model that joins the guest's SoftAP, one for each
 * host on the netdev that sends traffic.
 */
typedef struct EspWifiClient {
    bool used;
    MACAddr mac;
    EspWifiLinkState link;
    EspWifiRsnKeys keys;
    int64_t last_attempt;
    GQueue pending;             /* Ethernet frames waiting for association */
} EspWifiClient;

/* The guest's own SoftAP, learnt from the beacons it transmits. */
typedef struct EspWifiGuestAp {
    bool valid;
    MACAddr bssid;
    char ssid[33];
    uint32_t channel;
    bool rsn;
    uint8_t akm;                /* AKM the emulated clients use */
    uint16_t rsn_caps;          /* PMF bits the emulated clients announce */
    uint8_t pmk[32];
} EspWifiGuestAp;

typedef struct EspWifiTxSlot {
    bool busy;
    bool wait_ack;
    int64_t deadline;           /* completion time, or ACK timeout */
    bool ok;                    /* false: some MPDU could not be sent */
    unsigned pending;           /* wmediumd: MPDUs waiting for TX status */
    unsigned n_ampdu;           /* MPDUs in an A-MPDU, 0: not one */
    uint32_t ba;                /* bit n: MPDU n acknowledged */
    uint16_t mpdu_seq[ESP_WIFI_AMPDU_MAX];  /* of the MPDUs */
    uint8_t ampdu_tid;
    uint64_t cookie;            /* wmediumd: cookie of the last MPDU */
} EspWifiTxSlot;

/* Model state of a hardware key entry; the key itself lives in the registers */
typedef struct EspWifiKey {
    uint64_t rx_pn[ESP_WIFI_KEY_RX_PN];
    uint8_t aes_key[16];
    void *aes;                  /* QCryptoCipher for aes_key */
} EspWifiKey;

/*
 * A 40 MHz BSS: the PLL is tuned to the centre, but the medium carries its
 * frames on the primary channel.
 */
typedef struct EspWifiHt40 {
    uint8_t bssid[6];
    uint8_t primary;            /* 0: unused entry */
    uint8_t centre;
} EspWifiHt40;

#define ESP_WIFI_HT40_MAX       8

/* Client of a wmediumd API socket (esp_wifi_wmediumd.c) */
typedef struct EspWifiWmd {
    CharBackend chr;
    bool connected;
    GByteArray *rx;             /* partial message from wmediumd */
    uint64_t cookie;
    uint8_t radio[6];           /* address the frames are sent with */
    uint8_t extra[2][6];        /* addresses added to the station */
    bool extra_valid[2];
    EspWifiHt40 ht40[ESP_WIFI_HT40_MAX];
    unsigned ht40_next;
    QEMUTimer *beacon_timer;    /* repeats the SoftAP's beacon */
    int64_t beacon_last;        /* when the driver last sent one */
    uint32_t beacon_chan;       /* and on which channel */
    size_t beacon_len;
    uint8_t beacon[512];
} EspWifiWmd;

struct EspWifiState {
    SysBusDevice parent_obj;

    MemoryRegion mac_iomem;
    EspWifiRadioRegion radio[ESP_WIFI_RADIO_MAX];
    qemu_irq irq;

    uint32_t *mac_regs;
    uint8_t *tx_buf;            /* frame or A-MPDU being transmitted */
    unsigned long *unimp_seen;

    uint32_t raw_int;
    uint32_t rx_cur;            /* next descriptor, 0 when the chain ran out */
    uint8_t own_mac[2][6];
    uint32_t channel;
    uint16_t seq;

    QSIMPLEQ_HEAD(, EspWifiFrame) rx_queue;
    unsigned rx_queue_len;
    QEMUTimer *rx_timer;
    QEMUTimer *beacon_timer;
    QEMUTimer *ack_timer;
    QEMUTimer *client_timer;
    QEMUTimer *eapol_timer;
    EspWifiTxSlot tx[ESP_WIFI_TX_SLOTS];

    EspWifiAp ap[ESP_WIFI_MAX_APS];
    EspWifiClient client[ESP_WIFI_MAX_CLIENTS];
    EspWifiGuestAp guest_ap;

    EspWifiKey keys[ESP_WIFI_KEYS];
    EspWifiWmd wmd;

    NICConf conf;
    NICState *nic;
    char *client_psk;           /* passphrase of the guest's SoftAP */
    uint32_t ack_timeout_us;
    uint32_t beacon_interval_tu;
    uint32_t air_timeout_ms;
    uint64_t tsf_latch;         /* TSF snapshot the driver reads back */
};

struct EspWifiClass {
    SysBusDeviceClass parent_class;

    const EspWifiMacLayout *layout;
    const EspWifiRadioBlock *radio;
    unsigned n_radio;
    /* Size of the RX control header the chip writes in front of a frame */
    size_t rx_ctrl_size;
    /* Added to the RSSI in dBm to give the value in the RX control header */
    int rssi_offset;
    void (*fill_rx_ctrl)(EspWifiState *s, uint8_t *hdr, const EspWifiFrame *f);
    /* Is the MAC register at @addr one the model knows about */
    bool (*mac_reg_known)(hwaddr addr);
    uint64_t (*radio_read)(EspWifiState *s, const EspWifiRadioBlock *b,
                           hwaddr addr, uint64_t value);
    void (*radio_write)(EspWifiState *s, const EspWifiRadioBlock *b,
                        hwaddr addr, uint64_t value);
};

/*
 * Map the MAC and radio regions at their bus addresses. @map is called once
 * per region so that the machine can add aliases (e.g. the ESP32 AHB mirror).
 */
void esp_wifi_map(EspWifiState *s,
                  void (*map)(void *opaque, hwaddr base, MemoryRegion *mr),
                  void *opaque);

/*
 * Create the Wi-Fi device of a machine, or return NULL: when a -nic asks for
 * @type, or when -global gives the device an air chardev (wmediumd).
 */
DeviceState *esp_wifi_create(const char *type);

/* Channel the radio is tuned to, as decoded by the chip's radio_write hook */
void esp_wifi_set_channel(EspWifiState *s, uint32_t channel);

/* Shared with esp_wifi_air.c */
void esp_wifi_deliver(EspWifiState *s, const uint8_t *frame, size_t len,
                      int8_t rssi);
void esp_wifi_send_eth(EspWifiState *s, const uint8_t *buf, size_t len);
void esp_wifi_tx_complete(EspWifiState *s, int slot, bool acked);
uint64_t esp_wifi_tsf(EspWifiState *s);
const uint8_t *esp_wifi_own_mac(EspWifiState *s, int iface);
