/*
 * WiFi station and Ethernet, each to a DHCP lease.
 */
#include "test_smoke.h"

/* --------------- 10. WiFi connect + DHCP (only on chips with WiFi) --------------- */

#if SOC_WIFI_SUPPORTED
static SemaphoreHandle_t g_wifi_sem = NULL;
static bool g_wifi_connected = false;
static bool g_wifi_got_ip = false;
static esp_ip4_addr_t g_wifi_ip = {0};

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        g_wifi_connected = true;
        ESP_LOGI(TAG, "  WiFi: STA_CONNECTED");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "  WiFi: STA_DISCONNECTED");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        g_wifi_got_ip = true;
        g_wifi_ip = evt->ip_info.ip;
        ESP_LOGI(TAG, "  WiFi: GOT_IP " IPSTR, IP2STR(&evt->ip_info.ip));
        xSemaphoreGive(g_wifi_sem);
    }
}

bool test_wifi(void)
{
    const char *TN = "wifi";
    esp_err_t err;

    g_wifi_connected = false;
    g_wifi_got_ip = false;
    g_wifi_sem = xSemaphoreCreateBinary();
    if (!g_wifi_sem) {
        FAIL_MSG(TN, "create semaphore failed");
        return false;
    }

    /* NVS, netif, default event loop are bootstrapped once in app_main. */
    esp_netif_create_default_wifi_sta();

    /* Register event handlers */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    /* Init WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_wifi_init: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Configure STA */
    wifi_config_t sta_cfg = {
        .sta = {
            .ssid = "myssid",
            .password = "mypassword",
        },
    };
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_wifi_set_config: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Start + connect */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_wifi_start: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_wifi_connect: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Wait for GOT_IP (covers auth, assoc, WPA2 handshake, DHCP) */
    if (xSemaphoreTake(g_wifi_sem, pdMS_TO_TICKS(15000)) != pdTRUE) {
        FAIL_MSG(TN, "did not get IP within 15s (connected=%d)", g_wifi_connected);
        goto fail;
    }

    /* Validate */
    if (!g_wifi_connected) {
        FAIL_MSG(TN, "GOT_IP but connected flag not set");
        goto fail;
    }
    if (!g_wifi_got_ip) {
        FAIL_MSG(TN, "semaphore taken but got_ip flag not set");
        goto fail;
    }
    if (g_wifi_ip.addr == 0) {
        FAIL_MSG(TN, "IP address is 0.0.0.0");
        goto fail;
    }

    ESP_LOGI(TAG, "  WiFi connected, IP=" IPSTR, IP2STR(&g_wifi_ip));

    /* Cleanup */
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    vSemaphoreDelete(g_wifi_sem);
    return true;

fail:
    esp_wifi_stop();
    esp_wifi_deinit();
    vSemaphoreDelete(g_wifi_sem);
    return false;
}
#endif /* SOC_WIFI_SUPPORTED */

/* --------------- 10b. Ethernet connect + DHCP (chips with internal EMAC) ---------------
 *
 * Non-WiFi targets (P4, S31) exercise the DHCP path over the internal
 * DesignWare GMAC instead. Modelled on examples/ethernet/basic: generic
 * 802.3 PHY (auto-detected MDIO address) + default ESP32 EMAC config. The
 * emulator's user-net backend answers DHCP just like it does for WiFi. */
#if SOC_EMAC_SUPPORTED
static SemaphoreHandle_t g_eth_sem = NULL;
static esp_ip4_addr_t g_eth_ip = {0};

static void eth_got_ip_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    g_eth_ip = evt->ip_info.ip;
    ESP_LOGI(TAG, "  ETH: GOT_IP " IPSTR, IP2STR(&evt->ip_info.ip));
    xSemaphoreGive(g_eth_sem);
}

bool test_eth(void)
{
    const char *TN = "eth";

    g_eth_ip.addr = 0;
    g_eth_sem = xSemaphoreCreateBinary();
    if (!g_eth_sem) { FAIL_MSG(TN, "create semaphore failed"); return false; }

    /* Default MAC/PHY/EMAC config; generic PHY auto-detects its MDIO address. */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1; /* no PHY reset line in the emulator */
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (!mac || !phy) { FAIL_MSG(TN, "create MAC/PHY failed"); goto fail; }

    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&config, &eth_handle) != ESP_OK) {
        FAIL_MSG(TN, "driver install failed");
        goto fail;
    }

    /* Attach to a default Ethernet netif. */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)) != ESP_OK) {
        FAIL_MSG(TN, "netif attach failed");
        goto fail;
    }

    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_got_ip_handler, NULL);

    if (esp_eth_start(eth_handle) != ESP_OK) {
        FAIL_MSG(TN, "esp_eth_start failed");
        goto fail;
    }

    /* Wait for DHCP (covers link-up, MDIO PHY bring-up, descriptor DMA). */
    if (xSemaphoreTake(g_eth_sem, pdMS_TO_TICKS(15000)) != pdTRUE) {
        FAIL_MSG(TN, "did not get IP within 15s");
        goto fail;
    }
    if (g_eth_ip.addr == 0) { FAIL_MSG(TN, "IP address is 0.0.0.0"); goto fail; }

    ESP_LOGI(TAG, "  ETH connected, IP=" IPSTR, IP2STR(&g_eth_ip));
    vSemaphoreDelete(g_eth_sem);
    return true;

fail:
    vSemaphoreDelete(g_eth_sem);
    return false;
}
#endif /* SOC_EMAC_SUPPORTED */
