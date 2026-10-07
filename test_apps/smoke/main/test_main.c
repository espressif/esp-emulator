/*
 * esp-emu smoke app — the cross-chip regression gate.
 *
 * Each test is a `bool test_<name>(void)` in a topic file next to this one;
 * this file owns only the running order, the pass/fail counters, and the
 * machine-parseable footer the harness greps for:
 *
 *   TEST [name]: PASSED / FAILED
 *   RESULT_SUMMARY: passed=N failed=M target=<chip>
 *
 * Some cases deliberately reboot (see test_reset.c), so app_main dispatches on
 * the current phase before running anything.
 */
#include "test_smoke.h"

const char *TAG = "emu_test";

int g_pass_count = 0;
int g_fail_count = 0;

void app_main(void)
{
    bool resumed = phase_begin();

    ESP_LOGI(TAG, "=== ESP-EMU Comprehensive Test Suite ===");
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);
    if (resumed) {
        ESP_LOGI(TAG, "Resumed at phase %" PRIu32 " (reset reason %d)",
                 phase_current(), (int)esp_reset_reason());
    }

    /* One-time bootstrap shared by NVS, esp_event, esp_netif/WiFi tests. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    switch (phase_current()) {
    case PHASE_AFTER_SW:
        RUN_TEST("reset_reason_sw",    test_reset_reason_sw);
        phase_save(PHASE_AFTER_WDT);
        trip_task_wdt();
        return; /* not reached */
    case PHASE_AFTER_WDT:
        RUN_TEST("reset_reason_wdt",   test_reset_reason_wdt);
#if EMU_TEST_SLEEP
        phase_save(PHASE_AFTER_SLEEP);
        ESP_LOGI(TAG, "entering deep sleep (expect a timer wake)");
        esp_sleep_enable_timer_wakeup(50000);
        esp_deep_sleep_start();
        return; /* not reached */
    case PHASE_AFTER_SLEEP:
        RUN_TEST("reset_reason_deepsleep", test_reset_reason_deepsleep);
#endif
        goto report;
    default:
        break;
    }

    RUN_TEST("system_info",            test_system_info);
    RUN_TEST("printf",                 test_printf);
    RUN_TEST("gpio",                   test_gpio);
#if SOC_GPSPI_SUPPORTED
    RUN_TEST("spi_iomux",              test_spi_iomux);
#endif
    RUN_TEST("partition_table",        test_partition_table);
    RUN_TEST("spi_flash",              test_spi_flash);
    RUN_TEST("mmap",                   test_mmap);
    RUN_TEST("nvs",                    test_nvs);
    RUN_TEST("esp_timer",              test_esp_timer);
#if SOC_RMT_SUPPORTED
    RUN_TEST("rmt_tx",                 test_rmt_tx);
#endif
#if SOC_LEDC_SUPPORTED
    RUN_TEST("ledc",                   test_ledc);
#endif
#if SOC_PCNT_SUPPORTED
    RUN_TEST("pcnt",                   test_pcnt);
#endif
#if SOC_I2C_SUPPORTED
    RUN_TEST("i2c",                    test_i2c);
#endif
    RUN_TEST("freertos_task",          test_freertos_task);
    RUN_TEST("freertos_semaphore",     test_freertos_semaphore);
    RUN_TEST("freertos_queue",         test_freertos_queue);
    RUN_TEST("freertos_notification",  test_freertos_notification);
    RUN_TEST("freertos_mutex",         test_freertos_mutex);
    RUN_TEST("freertos_counting_sem",  test_freertos_counting_sem);
    RUN_TEST("freertos_event_group",   test_freertos_event_group);
    RUN_TEST("freertos_software_timer",test_freertos_software_timer);
    RUN_TEST("vtask_delay_until",      test_vtask_delay_until);
    RUN_TEST("pthread",                test_pthread);
    RUN_TEST("esp_event_custom",       test_esp_event_custom);
    RUN_TEST("esp_random",             test_esp_random);
    RUN_TEST("crypto_sha256_dma",      test_crypto_sha256_dma);
    RUN_TEST("crypto_aes128",          test_crypto_aes128);
    RUN_TEST("crypto_hmac_sha256",     test_crypto_hmac_sha256);
    RUN_TEST("crypto_ecdsa_p256",      test_crypto_ecdsa_p256);
#if SOC_ECDSA_SUPPORTED
    /* export_pubkey runs FIRST of the hardware-ECDSA cases on purpose: it must
     * be the boot's first ECDSA op for the cold-CONF.SOFTWARE_SET_Z path to be
     * exercised. Any preceding sign or verify sets that bit (they pass
     * sha_mode) and leaves it set, since the LL read-modify-writes CONF. */
#if SOC_ECDSA_SUPPORT_EXPORT_PUBKEY
    RUN_TEST("crypto_ecdsa_export_pubkey", test_crypto_ecdsa_export_pubkey);
#endif
    RUN_TEST("crypto_ecdsa_hw_verify", test_crypto_ecdsa_hw_verify);
    RUN_TEST("crypto_ecdsa_sign",      test_crypto_ecdsa_sign);
#endif
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    RUN_TEST("smp_pinned",             test_smp_pinned);
#endif
#if SOC_WIFI_SUPPORTED
    RUN_TEST("wifi",                   test_wifi);
#endif
#if SOC_EMAC_SUPPORTED
    RUN_TEST("eth",                    test_eth);
#endif
#if CONFIG_BT_NIMBLE_ENABLED
    RUN_TEST("ble",                    test_ble);
#endif

#if EMU_TEST_SLEEP
    RUN_TEST("light_sleep",            test_light_sleep);
#endif

    /* Cold phase done — hand off to the reboot-driven reset-reason cases. */
    phase_save(PHASE_AFTER_SW);
    ESP_LOGI(TAG, "restarting to check reset reasons");
    esp_restart();

report:
    ESP_LOGI(TAG, "=== Test Suite Complete ===");
    ESP_LOGI(TAG, "Results: %d passed, %d failed", g_pass_count, g_fail_count);
    /* Single-line machine-parseable footer for --exit-on / CI grep. */
    printf("RESULT_SUMMARY: passed=%d failed=%d target=%s\n",
           g_pass_count, g_fail_count, CONFIG_IDF_TARGET);

    /* Keep running briefly so the emulator can capture the final output */
    vTaskDelay(pdMS_TO_TICKS(100));
}
