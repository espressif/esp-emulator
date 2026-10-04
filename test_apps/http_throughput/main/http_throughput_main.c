/*
 * HTTP throughput benchmark for esp-emu.
 *
 * Brings up the NIC (WiFi STA on C3/C6, internal EMAC on P4/S31), downloads a
 * large payload from the host's local HTTP server via the user-net gateway
 * (192.168.4.1 -> host 127.0.0.1), and reports throughput measured in emulated
 * time (esp_timer), which is stable across host load.
 *
 * Prints exactly one machine-readable line on success:
 *   HTTP_TPUT: bytes=<N> us=<T> mbit_s=<R> target=<chip> link=<wifi|eth>
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#define TPUT_PORT 8070
#define TPUT_URL  "http://192.168.4.1:8070/bigfile"

static const char *TAG = "http_tput";

#if SOC_WIFI_SUPPORTED
#define LINK_NAME "wifi"
#else
#define LINK_NAME "eth"
#endif

static SemaphoreHandle_t s_got_ip;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "GOT_IP " IPSTR, IP2STR(&evt->ip_info.ip));
    xSemaphoreGive(s_got_ip);
}

/* ------------------------------------------------------------------ link up */
#if SOC_WIFI_SUPPORTED
#include "esp_wifi.h"

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    }
}

static bool link_up(void)
{
    esp_netif_create_default_wifi_sta();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    wifi_config_t sta = {
        .sta = { .ssid = "myssid", .password = "mypassword" },
    };
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;
    return true;
}

#else  /* internal EMAC (P4 / S31) */
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"

static bool link_up(void)
{
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;  /* no PHY reset line in the emulator */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (!mac || !phy) return false;

    esp_eth_handle_t eth = NULL;
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&cfg, &eth) != ESP_OK) return false;

    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    if (esp_netif_attach(netif, esp_eth_new_netif_glue(eth)) != ESP_OK) return false;
    if (esp_eth_start(eth) != ESP_OK) return false;
    return true;
}
#endif

/* ----------------------------------------------------------------- download */
static bool download_and_measure(void)
{
    esp_http_client_config_t cfg = {
        .url = TPUT_URL,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { ESP_LOGE(TAG, "client init failed"); return false; }

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return false;
    }
    int64_t content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    ESP_LOGI(TAG, "HTTP %d, content-length=%lld", status, content_len);
    if (status != 200) {
        esp_http_client_cleanup(cli);
        return false;
    }

    static char buf[4096];
    int64_t total = 0;
    int64_t t0 = esp_timer_get_time();
    while (1) {
        int r = esp_http_client_read(cli, buf, sizeof(buf));
        if (r < 0) { ESP_LOGE(TAG, "read error after %lld bytes", total); break; }
        if (r == 0) break;  /* complete */
        total += r;
    }
    int64_t t1 = esp_timer_get_time();
    int64_t us = t1 - t0;

    bool complete = esp_http_client_is_complete_data_received(cli) ||
                    (content_len > 0 && total == content_len);
    esp_http_client_cleanup(cli);
    /* bytes*8 / microseconds == Mbit/s (1 bit/us == 1 Mbit/s) */
    double mbit = us > 0 ? (double)total * 8.0 / (double)us : 0.0;
    ESP_LOGI(TAG, "downloaded %lld bytes in %lld us (complete=%d)", total, us, complete);
    printf("HTTP_TPUT: bytes=%lld us=%lld mbit_s=%.2f target=%s link=%s\n",
           total, us, mbit, CONFIG_IDF_TARGET, LINK_NAME);
    return complete && total > 0;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_got_ip = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "bringing up %s link...", LINK_NAME);
    if (!link_up()) {
        printf("HTTP_TPUT_RESULT: FAILED (link bring-up)\n");
        return;
    }
    if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(20000)) != pdTRUE) {
        printf("HTTP_TPUT_RESULT: FAILED (no IP in 20s)\n");
        return;
    }

    bool ok = download_and_measure();
    printf("HTTP_TPUT_RESULT: %s\n", ok ? "PASSED" : "FAILED");
}
