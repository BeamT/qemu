/*
 * QEMU Bluetooth test: NimBLE peripheral on an external HCI controller
 *
 * The NimBLE host runs on the emulated chip and reaches the controller
 * through a UART (CONFIG_BT_NIMBLE_TRANSPORT_UART). It advertises as
 * "qemu-ble" and serves one GATT service:
 *
 *   - hello   read: "hello from QEMU"
 *   - echo    write, notify: each write is notified back to subscribers
 *   - secret  read only over an encrypted link, so reading it pairs
 *
 * Every step is logged with a "ble:" prefix for the test harness.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

#define DEVICE_NAME "qemu-ble"

/* 5a9e0000-1a2b-4c3d-8e4f-51454d55424c, "QEMUBL" in the last bytes */
#define TEST_UUID(n) BLE_UUID128_INIT(0x4c, 0x42, 0x55, 0x4d, 0x45, 0x51, \
    0x4f, 0x8e, 0x3d, 0x4c, 0x2b, 0x1a, (n), 0x00, 0x9e, 0x5a)

static const ble_uuid128_t svc_uuid = TEST_UUID(0x00);
static const ble_uuid128_t hello_uuid = TEST_UUID(0x01);
static const ble_uuid128_t echo_uuid = TEST_UUID(0x02);
static const ble_uuid128_t secret_uuid = TEST_UUID(0x03);

static const char hello[] = "hello from QEMU";
static const char secret[] = "paired with QEMU";

static uint8_t own_addr_type;
static uint16_t echo_handle;
static uint8_t echo_value[244];
static uint16_t echo_len;

static void advertise(void);

static int chr_access(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ble_uuid_cmp(uuid, &hello_uuid.u) == 0) {
        return os_mbuf_append(ctxt->om, hello, strlen(hello)) ?
               BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ble_uuid_cmp(uuid, &secret_uuid.u) == 0) {
        ESP_LOGI(TAG, "ble: secret read");
        return os_mbuf_append(ctxt->om, secret, strlen(secret)) ?
               BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ble_uuid_cmp(uuid, &echo_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return os_mbuf_append(ctxt->om, echo_value, echo_len) ?
                   BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        }
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(echo_value)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ble_hs_mbuf_to_flat(ctxt->om, echo_value, sizeof(echo_value),
                            &echo_len);
        ESP_LOGI(TAG, "ble: write %u bytes", echo_len);
        ble_gatts_chr_updated(echo_handle);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &hello_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                .uuid = &echo_uuid.u,
                .access_cb = chr_access,
                .val_handle = &echo_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_NOTIFY,
            }, {
                .uuid = &secret_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            }, {
                0,
            },
        },
    }, {
        0,
    },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
            ESP_LOGI(TAG, "ble: connect failed, status %d",
                     event->connect.status);
            advertise();
            break;
        }
        ble_gap_conn_find(event->connect.conn_handle, &desc);
        ESP_LOGI(TAG, "ble: connected to %02x:%02x:%02x:%02x:%02x:%02x",
                 desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                 desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                 desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "ble: disconnected, reason 0x%x",
                 event->disconnect.reason);
        advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "ble: mtu %d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == echo_handle) {
            ESP_LOGI(TAG, "ble: subscribed %d", event->subscribe.cur_notify);
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        ESP_LOGI(TAG, "ble: encryption status %d encrypted %d bonded %d",
                 event->enc_change.status, desc.sec_state.encrypted,
                 desc.sec_state.bonded);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The peer lost its keys: forget ours and pair again */
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name = (uint8_t *)DEVICE_NAME,
        .name_len = strlen(DEVICE_NAME),
        .name_is_complete = 1,
    };
    int rc;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc == 0) {
        rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params,
                               gap_event, NULL);
    }
    if (rc) {
        ESP_LOGE(TAG, "ble: advertising failed, rc %d", rc);
        return;
    }
    ESP_LOGI(TAG, "ble: advertising");
}

static void on_sync(void)
{
    uint8_t addr[6];

    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "ble: sync, address %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "ble: host reset, reason %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

void app_main(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                 BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(DEVICE_NAME));
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
}
