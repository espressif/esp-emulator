/*
 * esp_timer and FreeRTOS tick accuracy.
 */
#include "test_smoke.h"

/* --------------- 7. esp_timer --------------- */

static volatile bool g_oneshot_fired = false;
static SemaphoreHandle_t g_oneshot_sem = NULL;

static void oneshot_timer_cb(void *arg)
{
    g_oneshot_fired = true;
    xSemaphoreGiveFromISR(g_oneshot_sem, NULL);
}

static volatile int g_periodic_count = 0;
static SemaphoreHandle_t g_periodic_sem = NULL;

static void periodic_timer_cb(void *arg)
{
    g_periodic_count++;
    if (g_periodic_count >= 3) {
        xSemaphoreGiveFromISR(g_periodic_sem, NULL);
    }
}

bool test_esp_timer(void)
{
    const char *TN = "esp_timer";
    esp_err_t err;

    /* One-shot timer: 10ms */
    g_oneshot_fired = false;
    g_oneshot_sem = xSemaphoreCreateBinary();
    if (!g_oneshot_sem) {
        FAIL_MSG(TN, "failed to create oneshot semaphore");
        return false;
    }

    esp_timer_handle_t oneshot;
    esp_timer_create_args_t oneshot_args = {
        .callback = oneshot_timer_cb,
        .name = "oneshot",
    };
    err = esp_timer_create(&oneshot_args, &oneshot);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "create oneshot: %s", esp_err_to_name(err));
        vSemaphoreDelete(g_oneshot_sem);
        return false;
    }

    err = esp_timer_start_once(oneshot, 10000); /* 10ms */
    if (err != ESP_OK) {
        FAIL_MSG(TN, "start oneshot: %s", esp_err_to_name(err));
        esp_timer_delete(oneshot);
        vSemaphoreDelete(g_oneshot_sem);
        return false;
    }

    /* Wait up to 500ms */
    if (xSemaphoreTake(g_oneshot_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        FAIL_MSG(TN, "oneshot did not fire within 500ms");
        esp_timer_stop(oneshot);
        esp_timer_delete(oneshot);
        vSemaphoreDelete(g_oneshot_sem);
        return false;
    }
    if (!g_oneshot_fired) {
        FAIL_MSG(TN, "oneshot semaphore taken but flag not set");
        esp_timer_delete(oneshot);
        vSemaphoreDelete(g_oneshot_sem);
        return false;
    }
    ESP_LOGI(TAG, "  oneshot timer fired OK");
    esp_timer_delete(oneshot);
    vSemaphoreDelete(g_oneshot_sem);

    /* Periodic timer: 5ms period, wait for 3 firings */
    g_periodic_count = 0;
    g_periodic_sem = xSemaphoreCreateBinary();
    if (!g_periodic_sem) {
        FAIL_MSG(TN, "failed to create periodic semaphore");
        return false;
    }

    esp_timer_handle_t periodic;
    esp_timer_create_args_t periodic_args = {
        .callback = periodic_timer_cb,
        .name = "periodic",
    };
    err = esp_timer_create(&periodic_args, &periodic);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "create periodic: %s", esp_err_to_name(err));
        vSemaphoreDelete(g_periodic_sem);
        return false;
    }

    err = esp_timer_start_periodic(periodic, 5000); /* 5ms */
    if (err != ESP_OK) {
        FAIL_MSG(TN, "start periodic: %s", esp_err_to_name(err));
        esp_timer_delete(periodic);
        vSemaphoreDelete(g_periodic_sem);
        return false;
    }

    if (xSemaphoreTake(g_periodic_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        FAIL_MSG(TN, "periodic did not fire 3 times within 500ms");
        esp_timer_stop(periodic);
        esp_timer_delete(periodic);
        vSemaphoreDelete(g_periodic_sem);
        return false;
    }

    esp_timer_stop(periodic);
    ESP_LOGI(TAG, "  periodic timer count=%d", g_periodic_count);
    if (g_periodic_count < 3) {
        FAIL_MSG(TN, "expected >= 3 firings, got %d", g_periodic_count);
        esp_timer_delete(periodic);
        vSemaphoreDelete(g_periodic_sem);
        return false;
    }

    esp_timer_delete(periodic);
    vSemaphoreDelete(g_periodic_sem);
    return true;
}

/* --------------- 15. vTaskDelayUntil accuracy --------------- */

bool test_vtask_delay_until(void)
{
    const char *TN = "vtask_delay_until";
    const TickType_t period = pdMS_TO_TICKS(50);
    const int N = 10;

    int64_t t0 = esp_timer_get_time();
    TickType_t last = xTaskGetTickCount();
    for (int i = 0; i < N; i++) {
        vTaskDelayUntil(&last, period);
    }
    int64_t elapsed_us = esp_timer_get_time() - t0;
    int64_t expected_us = (int64_t)N * 50000;
    int64_t err_us = elapsed_us - expected_us;
    int64_t abs_err = err_us < 0 ? -err_us : err_us;

    ESP_LOGI(TAG, "  %d * 50ms: elapsed=%lld us, error=%lld us (%.1f%%)",
             N, (long long)elapsed_us, (long long)err_us,
             (double)err_us * 100.0 / (double)expected_us);

    /* Allow ±5% slack */
    if (abs_err * 20 > expected_us) {
        FAIL_MSG(TN, "drift > 5%%: elapsed=%lld expected=%lld",
                 (long long)elapsed_us, (long long)expected_us);
        return false;
    }
    return true;
}
