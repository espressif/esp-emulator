/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * esp-emu APSTA gateway test_app.
 *
 * Runs its own soft AP on IDF's default 192.168.4.1/24 and, beside it, a
 * station on the emulator's AP. Once the station has a lease, with its own AP
 * still up, it connects to the gateway the lease names, sends "ping" and
 * expects "pong" from the host service the suite runs behind it. The gateway
 * is read from the lease, not assumed, so the app holds for any subnet the
 * emulator hands out that is not its own AP's.
 *
 *   TEST apsta_gateway: PASSED / FAILED
 *   RESULT_SUMMARY: passed=N failed=M target=<chip>
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

static const char *TAG = "apsta_gateway";

#define AP_SSID   "esp-emu-apsta"
#define STA_SSID  "myssid"
#define STA_PASS  "mypassword"
#define HOST_PORT 18490

#define BIT_GOT_IP BIT0

static EventGroupHandle_t s_events;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

/* Connect to gw:HOST_PORT, send "ping", expect "pong". */
static bool ping_gateway(esp_ip4_addr_t gw)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return false;
    }
    struct timeval tv = { .tv_sec = 10 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(HOST_PORT),
        .sin_addr.s_addr = gw.addr,
    };
    bool ok = false;
    char buf[8] = { 0 };
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) != 0) {
        ESP_LOGE(TAG, "connect " IPSTR ":%d: errno %d", IP2STR(&gw), HOST_PORT, errno);
    } else if (send(s, "ping", 4, 0) != 4) {
        ESP_LOGE(TAG, "send: errno %d", errno);
    } else {
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        ok = n == 4 && memcmp(buf, "pong", 4) == 0;
        if (!ok) {
            ESP_LOGE(TAG, "recv %d bytes, errno %d", n, errno);
        }
    }
    close(s);
    return ok;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    s_events = xEventGroupCreate();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    wifi_config_t sta_cfg = {
        .sta = {
            .ssid = STA_SSID,
            .password = STA_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    int pass = 0, fail = 0;
    bool got_ip = xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(60000)) & BIT_GOT_IP;
    esp_netif_ip_info_t ap_ip = { 0 }, sta_ip = { 0 };
    esp_netif_get_ip_info(ap, &ap_ip);
    esp_netif_get_ip_info(sta, &sta_ip);
    ESP_LOGI(TAG, "ap " IPSTR ", sta " IPSTR " gw " IPSTR,
             IP2STR(&ap_ip.ip), IP2STR(&sta_ip.ip), IP2STR(&sta_ip.gw));
    if (got_ip && ping_gateway(sta_ip.gw)) {
        printf("TEST apsta_gateway: PASSED\n");
        pass++;
    } else {
        printf("TEST apsta_gateway: FAILED\n");
        fail++;
    }
    printf("RESULT_SUMMARY: passed=%d failed=%d target=%s\n", pass, fail, CONFIG_IDF_TARGET);
}
