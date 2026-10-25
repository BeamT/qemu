/*
 * QEMU Wi-Fi test: reconnect after esp_restart()
 *
 * Boot 0 joins the AP, keeps UDP traffic in flight and restarts the chip in
 * the middle of it. Boot 1 must join again and get an address.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#define GOT_IP BIT0

static EventGroupHandle_t s_events;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START ||
                               id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, GOT_IP);
    }
}

static uint32_t next_boot(void)
{
    nvs_handle_t h;
    uint32_t boot = 0;

    ESP_ERROR_CHECK(nvs_open("test", NVS_READWRITE, &h));
    nvs_get_u32(h, "boot", &boot);
    ESP_ERROR_CHECK(nvs_set_u32(h, "boot", boot + 1));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    return boot;
}

void app_main(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wc = { .sta = { .ssid = "Espressif" } };
    uint32_t boot;

    if (nvs_flash_init() != ESP_OK) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    boot = next_boot();
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (!(xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdTRUE,
                              pdMS_TO_TICKS(30000)) & GOT_IP)) {
        printf("RESTART_TEST FAIL boot=%lu no ip\n", (unsigned long)boot);
        return;
    }
    printf("RESTART_TEST boot=%lu got ip\n", (unsigned long)boot);
    if (boot > 0) {
        printf("RESTART_TEST PASS\n");
        return;
    }

    /* Restart with frames still queued in the MAC */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(9),
        .sin_addr.s_addr = inet_addr("10.0.2.2"),
    };
    char buf[512] = { 0 };
    for (int i = 0; i < 200; i++) {
        sendto(sock, buf, sizeof(buf), 0, (struct sockaddr *)&to, sizeof(to));
        if (i == 150) {
            printf("RESTART_TEST restarting\n");
            fflush(stdout);
            esp_restart();
        }
        vTaskDelay(1);
    }
}
