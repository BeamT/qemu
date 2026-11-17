/*
 * QEMU Wi-Fi test: a station and SoftAP driven from the serial console, for
 * tests against real peers on a wmediumd medium (tests/esp_wifi/run_air.py).
 *
 * Commands, one per line:
 *   scan                       list the access points heard
 *   join <ssid> <passphrase>   join a WPA2 network ("-" for an open one)
 *   leave                      disconnect
 *   ping <ip> [count]          ICMP echo from the guest
 *   ap <ssid> <passphrase> <channel>
 *                              run a WPA2 SoftAP (PMF capable, not required)
 *   sta                        back to station only
 *
 * An HTTP server answers GET / on port 80 on every interface. Everything the
 * test looks for is printed on lines starting with "AIR ".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"
#include "esp_idf_version.h"

/* Renamed in IDF 6.0 */
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
#define IP_EVENT_ASSIGNED_IP_TO_CLIENT  IP_EVENT_AP_STAIPASSIGNED
typedef ip_event_ap_staipassigned_t ip_event_assigned_ip_to_client_t;
#endif

static SemaphoreHandle_t s_ping_done;
static int s_ping_ok;
static int s_join_retries;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t *e = data;
        printf("AIR sta connected ssid=%.*s channel=%d\n", e->ssid_len,
               (const char *)e->ssid, e->channel);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *e = data;
        printf("AIR sta disconnected reason=%d\n", e->reason);
        /* Like real firmware, try again if the scan missed the network */
        if (e->reason == WIFI_REASON_NO_AP_FOUND && s_join_retries > 0) {
            s_join_retries--;
            esp_wifi_connect();
        }
        break;
    }
    case WIFI_EVENT_AP_START:
        printf("AIR ap started\n");
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = data;
        printf("AIR ap join " MACSTR "\n", MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = data;
        printf("AIR ap leave " MACSTR " reason=%d\n", MAC2STR(e->mac),
               e->reason);
        break;
    }
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        printf("AIR sta got ip " IPSTR "\n", IP2STR(&e->ip_info.ip));
    } else if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t *e = data;
        printf("AIR ap lease " IPSTR "\n", IP2STR(&e->ip));
    }
}

static esp_err_t http_get(httpd_req_t *req)
{
    printf("AIR http served\n");
    return httpd_resp_sendstr(req, "AIR hello\n");
}

static void scan(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    uint16_t n = 0;
    wifi_ap_record_t *recs;

    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        printf("AIR scan failed\n");
        return;
    }
    esp_wifi_scan_get_ap_num(&n);
    recs = calloc(n ? n : 1, sizeof(*recs));
    esp_wifi_scan_get_ap_records(&n, recs);
    for (int i = 0; i < n; i++) {
        printf("AIR scan ssid=%s channel=%d rssi=%d auth=%d\n",
               (const char *)recs[i].ssid, recs[i].primary, recs[i].rssi,
               recs[i].authmode);
    }
    printf("AIR scan done n=%d\n", n);
    free(recs);
}

static void join(const char *ssid, const char *psk)
{
    wifi_config_t wc = { 0 };
    wifi_mode_t mode;

    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (strcmp(psk, "-")) {
        strlcpy((char *)wc.sta.password, psk, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_join_retries = 3;
    printf("AIR join %s\n", esp_err_to_name(esp_wifi_connect()));
}

static void softap(const char *ssid, const char *psk, int channel)
{
    wifi_config_t wc = { 0 };

    strlcpy((char *)wc.ap.ssid, ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(ssid);
    wc.ap.channel = channel;
    wc.ap.max_connection = 4;
    if (strcmp(psk, "-")) {
        strlcpy((char *)wc.ap.password, psk, sizeof(wc.ap.password));
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
        wc.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }
    wc.ap.pmf_cfg.required = false;
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_AP);
    printf("AIR ap %s\n", esp_err_to_name(esp_wifi_set_config(WIFI_IF_AP,
                                                               &wc)));
}

static void ping_success(esp_ping_handle_t h, void *arg)
{
    s_ping_ok++;
}

static void ping_end(esp_ping_handle_t h, void *arg)
{
    xSemaphoreGive(s_ping_done);
}

static void ping(const char *ip, int count)
{
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    esp_ping_callbacks_t cb = {
        .on_ping_success = ping_success,
        .on_ping_end = ping_end,
    };
    esp_ping_handle_t h;
    ip4_addr_t a;

    if (!inet_aton(ip, &a)) {
        printf("AIR ping bad address\n");
        return;
    }
    ipaddr_aton(ip, &pc.target_addr);
    pc.count = count;
    pc.interval_ms = 500;
    s_ping_ok = 0;
    if (esp_ping_new_session(&pc, &cb, &h) != ESP_OK) {
        printf("AIR ping failed\n");
        return;
    }
    esp_ping_start(h);
    xSemaphoreTake(s_ping_done, portMAX_DELAY);
    esp_ping_delete_session(h);
    printf("AIR ping ok=%d sent=%d\n", s_ping_ok, count);
}

static void console(void)
{
    char line[160];

    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 512, 0,
                                        0, NULL, 0));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                          ESP_LINE_ENDINGS_CR);
    setvbuf(stdin, NULL, _IONBF, 0);
    printf("AIR ready\n");

    while (fgets(line, sizeof(line), stdin)) {
        char *argv[5] = { 0 };
        char *save = NULL;
        int argc = 0;

        for (char *t = strtok_r(line, " \r\n", &save); t && argc < 5;
             t = strtok_r(NULL, " \r\n", &save)) {
            argv[argc++] = t;
        }
        if (!argc) {
            continue;
        }
        if (!strcmp(argv[0], "scan")) {
            scan();
        } else if (!strcmp(argv[0], "join") && argc == 3) {
            join(argv[1], argv[2]);
        } else if (!strcmp(argv[0], "leave")) {
            s_join_retries = 0;
            esp_wifi_disconnect();
        } else if (!strcmp(argv[0], "ping") && argc >= 2) {
            ping(argv[1], argc > 2 ? atoi(argv[2]) : 4);
        } else if (!strcmp(argv[0], "sta")) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            printf("AIR sta mode\n");
        } else if (!strcmp(argv[0], "ap") && argc == 4) {
            softap(argv[1], argv[2], atoi(argv[3]));
        } else {
            printf("AIR unknown command %s\n", argv[0]);
        }
    }
}

void app_main(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = http_get };
    httpd_handle_t srv;

    if (nvs_flash_init() != ESP_OK) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_ping_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               on_ip, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));
    httpd_register_uri_handler(srv, &root);
    console();
}
