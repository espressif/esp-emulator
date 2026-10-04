/*
 * Reset reasons, watchdog and sleep (the reboot phases).
 */
#include "test_smoke.h"

/* --------------- reset reasons, watchdog, sleep ---------------
 *
 * These cases have to survive a reboot, so the app runs as a small state
 * machine: each phase asserts the reset reason it was put into, arms the next
 * one, and reboots. Progress and the running counters live in RTC_NOINIT
 * memory, which (unlike RTC_DATA) the bootloader does not re-initialise on a
 * warm boot — the magic word is what distinguishes "cold boot" from "resumed".
 *
 * The emulator harness runs with `allow_restart`, so the reboots are expected;
 * only the final phase prints RESULT_SUMMARY.
 */
#define PHASE_MAGIC 0x50484153u /* "PHAS" */

RTC_NOINIT_ATTR static uint32_t s_phase_magic;
RTC_NOINIT_ATTR static uint32_t s_phase;
RTC_NOINIT_ATTR static uint32_t s_saved_pass;
RTC_NOINIT_ATTR static uint32_t s_saved_fail;

uint32_t phase_current(void)
{
    return s_phase;
}

bool phase_begin(void)
{
    bool resumed = (s_phase_magic == PHASE_MAGIC);
    if (!resumed) {
        s_phase_magic = PHASE_MAGIC;
        s_phase = PHASE_COLD;
        s_saved_pass = 0;
        s_saved_fail = 0;
    }
    g_pass_count = s_saved_pass;
    g_fail_count = s_saved_fail;
    return resumed;
}

void phase_save(uint32_t next)
{
    s_phase = next;
    s_saved_pass = g_pass_count;
    s_saved_fail = g_fail_count;
}

static bool reset_reason_is(const char *tn, esp_reset_reason_t want, const char *want_name)
{
    esp_reset_reason_t got = esp_reset_reason();
    if (got != want) {
        FAIL_MSG(tn, "reset reason %d, expected %s (%d)", (int)got, want_name, (int)want);
        return false;
    }
    return true;
}

bool test_reset_reason_sw(void)
{
    return reset_reason_is("reset_reason_sw", ESP_RST_SW, "ESP_RST_SW");
}

bool test_reset_reason_wdt(void)
{
    /* IDF reports a task-WDT-induced panic as ESP_RST_TASK_WDT where the SoC
     * distinguishes it and ESP_RST_PANIC otherwise; both mean "the watchdog
     * fired and the panic handler rebooted us", which is what is under test. */
    esp_reset_reason_t got = esp_reset_reason();
    if (got != ESP_RST_TASK_WDT && got != ESP_RST_PANIC && got != ESP_RST_INT_WDT) {
        FAIL_MSG("reset_reason_wdt", "reset reason %d, expected a watchdog/panic reset",
                 (int)got);
        return false;
    }
    return true;
}

#if EMU_TEST_SLEEP
bool test_reset_reason_deepsleep(void)
{
    if (!reset_reason_is("reset_reason_deepsleep", ESP_RST_DEEPSLEEP, "ESP_RST_DEEPSLEEP")) {
        return false;
    }
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        FAIL_MSG("reset_reason_deepsleep", "wakeup cause %d, expected TIMER",
                 (int)esp_sleep_get_wakeup_cause());
        return false;
    }
    return true;
}

bool test_light_sleep(void)
{
    const char *TN = "light_sleep";
    /* Light sleep returns to the same instruction stream, so it can be checked
     * inline: arm a timer wake, sleep, and confirm both the cause and that
     * emulated time actually advanced by roughly the requested amount. */
    const int64_t req_us = 50000;
    if (esp_sleep_enable_timer_wakeup(req_us) != ESP_OK) {
        FAIL_MSG(TN, "esp_sleep_enable_timer_wakeup failed");
        return false;
    }
    int64_t before = esp_timer_get_time();
    esp_err_t err = esp_light_sleep_start();
    int64_t slept = esp_timer_get_time() - before;
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_light_sleep_start failed: %s", esp_err_to_name(err));
        return false;
    }
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        FAIL_MSG(TN, "wakeup cause %d, expected TIMER", (int)esp_sleep_get_wakeup_cause());
        return false;
    }
    /* Generous bounds: this asserts the timer ran, not its accuracy. */
    if (slept < req_us / 2 || slept > req_us * 8) {
        FAIL_MSG(TN, "slept %" PRId64 " us, expected ~%" PRId64, slept, req_us);
        return false;
    }
    return true;
}
#endif /* EMU_TEST_SLEEP */

/* Hang with the task WDT armed so it fires and reboots us. */
void trip_task_wdt(void)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    /* Already initialised by CONFIG_ESP_TASK_WDT_INIT on some configs. */
    if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_reconfigure(&cfg);
    }
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "arming task WDT (expect a watchdog reset)");
    while (true) {
        /* Deliberately never feed it. */
    }
}
