/*
 * QEMU Wi-Fi test: ESP-NOW delivery status
 *
 * Two instances with MACs 02:00:00:00:01:00 and 02:00:00:00:02:00 send
 * unicast frames to each other, and to 02:00:00:00:99:00, which does not
 * exist. (IDF gives the other interfaces the following addresses, so the
 * base addresses must not be consecutive.) Every frame to the peer must be acknowledged, every frame to the
 * missing station must fail, and frames from the peer must arrive.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define COUNT 10
#ifdef CONFIG_ESPNOW_TEST_ENCRYPT
#define ENCRYPT true
#else
#define ENCRYPT false
#endif
#define MAX_TRIES 200

static QueueHandle_t s_status;
static volatile int s_rx;

static void on_sent(const esp_now_send_info_t *info,
                    esp_now_send_status_t status)
{
    xQueueSend(s_status, &status, 0);
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data,
                    int len)
{
    s_rx++;
}

static void add_peer(const uint8_t *mac, bool encrypt)
{
    esp_now_peer_info_t peer = {
        .channel = 1,
        .ifidx = WIFI_IF_STA,
        .encrypt = encrypt,
    };

    memcpy(peer.peer_addr, mac, 6);
    memcpy(peer.lmk, "lmk1234567890123", 16);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

static esp_now_send_status_t send_one(const uint8_t *mac)
{
    esp_now_send_status_t status = ESP_NOW_SEND_FAIL;
    static const char msg[] = "qemu esp-now test";

    if (esp_now_send(mac, (const uint8_t *)msg, sizeof(msg)) != ESP_OK ||
        !xQueueReceive(s_status, &status, pdMS_TO_TICKS(1000))) {
        return ESP_NOW_SEND_FAIL;
    }
    return status;
}

void app_main(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    uint8_t own[6], peer[6] = { 2, 0, 0, 0, 0, 0 };
    static const uint8_t missing[6] = { 2, 0, 0, 0, 0x99, 0 };
    int ok = 0, fail = 0, tries;

    if (nvs_flash_init() != ESP_OK) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_status = xQueueCreate(4, sizeof(esp_now_send_status_t));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_set_pmk((const uint8_t *)"pmk1234567890123"));

    esp_read_mac(own, ESP_MAC_WIFI_STA);
    peer[4] = own[4] == 1 ? 2 : 1;
    add_peer(peer, ENCRYPT);
    add_peer(missing, false);
    printf("ESPNOW_TEST own " MACSTR " peer " MACSTR "\n", MAC2STR(own),
           MAC2STR(peer));

    /* The peer may still be booting: retry until it acknowledges */
    for (tries = 0; tries < MAX_TRIES && ok < COUNT; tries++) {
        if (send_one(peer) == ESP_NOW_SEND_SUCCESS) {
            ok++;
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    for (tries = 0; tries < COUNT; tries++) {
        if (send_one(missing) == ESP_NOW_SEND_FAIL) {
            fail++;
        }
    }
    /* Give the peer time to finish its own sends */
    vTaskDelay(pdMS_TO_TICKS(3000));
    printf("ESPNOW_TEST peer_ok=%d/%d missing_fail=%d/%d rx=%d\n", ok, COUNT,
           fail, COUNT, s_rx);
    printf("ESPNOW_TEST %s\n", ok == COUNT && fail == COUNT && s_rx >= COUNT ?
           "PASS" : "FAIL");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
