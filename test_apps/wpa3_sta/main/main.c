/*
 * esp-emu WPA3 station test_app.
 *
 * Connects to the soft AP with each SAE variant the ESP-IDF supplicant
 * offers, then once more through PMKSA caching, and prints the same
 * machine-parseable footer as test_apps/smoke:
 *
 *   TEST <name>: PASSED / FAILED
 *   RESULT_SUMMARY: passed=N failed=M target=<chip>
 *
 * Every phase but the reconnect one tears WiFi down completely so the
 * supplicant forgets its PMKSA and runs a fresh SAE exchange.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "wpa3_sta";

#define BIT_CONNECTED    BIT0
#define BIT_GOT_IP       BIT1
#define BIT_DISCONNECTED BIT2

static EventGroupHandle_t s_events;
static wifi_auth_mode_t s_connected_authmode;
static uint8_t s_disconnect_reason;
static int s_pass, s_fail;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *ev = data;
        s_connected_authmode = ev->authmode;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        s_disconnect_reason = ev->reason;
        ESP_LOGW(TAG, "disconnected, reason %d", ev->reason);
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static const char *authmode_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
    default: return "other";
    }
}

typedef struct {
    const char *name;
    wifi_auth_mode_t threshold;
    bool pmf_required;
    wifi_sae_pwe_method_t pwe;
    /* Reconnect with the supplicant still up instead of a fresh init. */
    bool reconnect_only;
} phase_t;

static const phase_t PHASES[] = {
    { "wpa3_h2e",            WIFI_AUTH_WPA3_PSK, true,  WPA3_SAE_PWE_HASH_TO_ELEMENT, false },
    { "wpa3_hunt_and_peck",  WIFI_AUTH_WPA3_PSK, true,  WPA3_SAE_PWE_HUNT_AND_PECK,   false },
    { "wpa3_both",           WIFI_AUTH_WPA3_PSK, true,  WPA3_SAE_PWE_BOTH,            false },
    { "wpa3_pmksa_reconnect",WIFI_AUTH_WPA3_PSK, true,  WPA3_SAE_PWE_BOTH,            true  },
    { "wpa2_threshold_pmf",  WIFI_AUTH_WPA2_PSK, true,  WPA3_SAE_PWE_BOTH,            false },
};

static bool wait_bits(EventBits_t bits, int ms)
{
    return (xEventGroupWaitBits(s_events, bits, pdTRUE, pdFALSE, pdMS_TO_TICKS(ms)) & bits) != 0;
}

static bool connect_and_wait(const phase_t *ph)
{
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_GOT_IP | BIT_DISCONNECTED);
    s_disconnect_reason = 0;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        return false;
    }
    if (!wait_bits(BIT_GOT_IP, 20000)) {
        ESP_LOGE(TAG, "%s: no IP within 20s (last disconnect reason %d)", ph->name, s_disconnect_reason);
        return false;
    }
    ESP_LOGI(TAG, "%s: connected, authmode %s", ph->name, authmode_name(s_connected_authmode));
    if (ph->threshold == WIFI_AUTH_WPA3_PSK &&
        s_connected_authmode != WIFI_AUTH_WPA3_PSK &&
        s_connected_authmode != WIFI_AUTH_WPA2_WPA3_PSK) {
        ESP_LOGE(TAG, "%s: expected a WPA3 association, got %s", ph->name, authmode_name(s_connected_authmode));
        return false;
    }
    return true;
}

static bool disconnect_and_wait(void)
{
    xEventGroupClearBits(s_events, BIT_DISCONNECTED);
    esp_wifi_disconnect();
    return wait_bits(BIT_DISCONNECTED, 5000);
}

static bool run_phase(const phase_t *ph)
{
    if (!ph->reconnect_only) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        wifi_config_t sta = { 0 };
        strcpy((char *)sta.sta.ssid, "myssid");
        strcpy((char *)sta.sta.password, "mypassword");
        sta.sta.threshold.authmode = ph->threshold;
        sta.sta.pmf_cfg.capable = true;
        sta.sta.pmf_cfg.required = ph->pmf_required;
        sta.sta.sae_pwe_h2e = ph->pwe;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    bool ok = connect_and_wait(ph);
    if (!disconnect_and_wait()) {
        ESP_LOGW(TAG, "%s: no disconnect event", ph->name);
    }
    return ok;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    ESP_LOGI(TAG, "=== ESP-EMU WPA3 station suite (%s) ===", CONFIG_IDF_TARGET);
    size_t n = sizeof(PHASES) / sizeof(PHASES[0]);
    for (size_t i = 0; i < n; i++) {
        const phase_t *ph = &PHASES[i];
        ESP_LOGI(TAG, "--- Running: %s ---", ph->name);
        bool ok = run_phase(ph);
        if (ok) {
            ESP_LOGI(TAG, "TEST %s: PASSED", ph->name);
            s_pass++;
        } else {
            ESP_LOGE(TAG, "TEST %s: FAILED", ph->name);
            s_fail++;
        }
        /* Keep WiFi up only when the next phase reconnects on the same supplicant. */
        bool keep = (i + 1 < n) && PHASES[i + 1].reconnect_only;
        if (!keep) {
            esp_wifi_stop();
            esp_wifi_deinit();
        }
    }

    printf("RESULT_SUMMARY: passed=%d failed=%d target=%s\n", s_pass, s_fail, CONFIG_IDF_TARGET);
    vTaskDelay(pdMS_TO_TICKS(100));
}
