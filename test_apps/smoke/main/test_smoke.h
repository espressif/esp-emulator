/*
 * Shared plumbing for the esp-emu smoke app.
 *
 * Every test lives in a topic file (test_storage.c, test_freertos.c, ...) and
 * exposes a single `bool test_<name>(void)` returning pass/fail. app_main
 * (test_main.c) is the only place that knows the running order.
 *
 * The includes are shared rather than per-file on purpose: the topic files are
 * a partition of one former translation unit, so one include set means adding
 * a test never turns into an include hunt.
 */
#pragma once

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include <pthread.h>
#include <limits.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_random.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
/* PSA, not the legacy mbedtls/sha256.h-style headers: those are gone in
 * mbedtls 4 (IDF master) and PSA is present from mbedtls 3.6 (IDF v5.5), so
 * this is the one crypto API that compiles across every release we test. */
#include "psa/crypto.h"

#include "driver/gpio.h"
#if SOC_RMT_SUPPORTED
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#endif
#if SOC_LEDC_SUPPORTED
#include "driver/ledc.h"
#endif
#if SOC_I2C_SUPPORTED
#include "driver/i2c_master.h"
#endif
#if SOC_GPSPI_SUPPORTED
#include "driver/spi_master.h"
#include "soc/spi_pins.h"
#endif

#include "esp_netif.h"
#include "esp_event.h"
#if SOC_WIFI_SUPPORTED
#include "esp_wifi.h"
#endif
#if SOC_EMAC_SUPPORTED
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#endif
#if CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#endif


/* Result counters, owned by test_main.c. The reset-reason phases save and
 * restore them across reboots (see test_reset.c). */
extern int g_pass_count;
extern int g_fail_count;
extern const char *TAG;

#define RUN_TEST(name, fn) do {                          \
    ESP_LOGI(TAG, "--- Running: %s ---", name);          \
    if (fn()) {                                          \
        ESP_LOGI(TAG, "TEST %s: PASSED", name);          \
        g_pass_count++;                                  \
    } else {                                             \
        ESP_LOGE(TAG, "TEST %s: FAILED", name);          \
        g_fail_count++;                                  \
    }                                                    \
} while (0)

#define FAIL_MSG(name, fmt, ...) do {                    \
    ESP_LOGE(TAG, "  FAIL %s: " fmt, name, ##__VA_ARGS__); \
} while (0)

/* --- test_storage.c --- */
bool test_nvs(void);
bool test_partition_table(void);
bool test_spi_flash(void);
bool test_mmap(void);

/* --- test_periph.c --- */
bool test_printf(void);
bool test_gpio(void);
#if SOC_GPSPI_SUPPORTED
bool test_spi_iomux(void);
#endif
#if SOC_PCNT_SUPPORTED
bool test_pcnt(void);
#endif
#if SOC_RMT_SUPPORTED
bool test_rmt_tx(void);
#endif
#if SOC_LEDC_SUPPORTED
bool test_ledc(void);
#endif
#if SOC_I2C_SUPPORTED
bool test_i2c(void);
#endif

/* --- test_timer.c --- */
bool test_esp_timer(void);
bool test_vtask_delay_until(void);

/* --- test_freertos.c --- */
bool test_freertos_task(void);
bool test_freertos_semaphore(void);
bool test_freertos_queue(void);
bool test_freertos_notification(void);
bool test_freertos_mutex(void);
bool test_freertos_counting_sem(void);
bool test_freertos_event_group(void);
bool test_freertos_software_timer(void);
bool test_pthread(void);
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
bool test_smp_pinned(void);
#endif

/* --- test_system.c --- */
bool test_system_info(void);
bool test_esp_event_custom(void);

/* --- test_net.c --- */
#if SOC_WIFI_SUPPORTED
bool test_wifi(void);
#endif
#if SOC_EMAC_SUPPORTED
bool test_eth(void);
#endif

/* --- test_ble.c --- */
#if CONFIG_BT_NIMBLE_ENABLED
bool test_ble(void);
#endif

/* --- test_crypto.c --- */
bool test_esp_random(void);
bool test_crypto_sha256_dma(void);
bool test_crypto_aes128(void);
bool test_crypto_hmac_sha256(void);
bool test_crypto_ecdsa_p256(void);
#if SOC_ECDSA_SUPPORTED
bool test_crypto_ecdsa_hw_verify(void);
bool test_crypto_ecdsa_sign(void);
#if SOC_ECDSA_SUPPORT_EXPORT_PUBKEY
bool test_crypto_ecdsa_export_pubkey(void);
#endif
#endif

/* --- test_reset.c --- *
 *
 * The reset-reason cases span reboots, so they are a small state machine
 * rather than plain test functions: app_main asks which phase it is in, runs
 * that phase's check, and arms the next one.
 */
enum {
    PHASE_COLD = 0,      /* first boot: run everything, then esp_restart() */
    PHASE_AFTER_SW,      /* expect ESP_RST_SW, then trip the task WDT */
    PHASE_AFTER_WDT,     /* expect a WDT/panic reset; report (or deep sleep) */
    PHASE_AFTER_SLEEP,   /* expect ESP_RST_DEEPSLEEP, then report */
};

/* The emulator models no sleep path today: there is no PMU/RTC wake plumbing,
 * so esp_light_sleep_start() never returns and esp_deep_sleep_start() never
 * wakes. The cases are written and ready — flip this to 1 once sleep lands. */
#define EMU_TEST_SLEEP 0

uint32_t phase_current(void);
/* True when this boot resumed a phase rather than starting cold; restores the
 * saved counters as a side effect. */
bool phase_begin(void);
void phase_save(uint32_t next);
void trip_task_wdt(void);

bool test_reset_reason_sw(void);
bool test_reset_reason_wdt(void);
#if EMU_TEST_SLEEP
bool test_reset_reason_deepsleep(void);
bool test_light_sleep(void);
#endif
