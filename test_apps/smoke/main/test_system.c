/*
 * Chip/system info and the esp_event loop.
 */
#include "test_smoke.h"

/* --------------- 9. System info --------------- */

bool test_system_info(void)
{
    const char *TN = "system_info";

    const char *idf_ver = esp_get_idf_version();
    ESP_LOGI(TAG, "  IDF version: %s", idf_ver);
    if (!idf_ver || strlen(idf_ver) == 0) {
        FAIL_MSG(TN, "IDF version is empty");
        return false;
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "  chip model=%d cores=%d features=0x%"PRIx32,
             chip.model, chip.cores, chip.features);

    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "  free heap: %u bytes", (unsigned)free_heap);
    if (free_heap == 0) {
        FAIL_MSG(TN, "free heap is 0");
        return false;
    }

    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "  reset reason: %d", (int)reason);

    return true;
}

/* --------------- 17. esp_event custom loop --------------- */

ESP_EVENT_DEFINE_BASE(EMU_TEST_EVENTS);

#define EMU_EVENT_PING 1
#define EMU_EVENT_PONG 2

static volatile int g_evt_specific = 0;
static volatile int g_evt_any_id   = 0;
static volatile int g_evt_any_base = 0;
static SemaphoreHandle_t g_evt_done;

static void evt_specific_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    g_evt_specific++;
}
static void evt_any_id_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    g_evt_any_id++;
    if (id == EMU_EVENT_PONG) xSemaphoreGive(g_evt_done);
}
static void evt_any_base_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    g_evt_any_base++;
}

bool test_esp_event_custom(void)
{
    const char *TN = "esp_event_custom";
    /* Default loop already created by app_main bootstrap. */
    g_evt_specific = g_evt_any_id = g_evt_any_base = 0;
    g_evt_done = xSemaphoreCreateBinary();

    esp_event_handler_register(EMU_TEST_EVENTS, EMU_EVENT_PING,    evt_specific_handler, NULL);
    esp_event_handler_register(EMU_TEST_EVENTS, ESP_EVENT_ANY_ID,  evt_any_id_handler,   NULL);
    esp_event_handler_register(ESP_EVENT_ANY_BASE, ESP_EVENT_ANY_ID, evt_any_base_handler, NULL);

    /* Post 2 PINGs + 1 PONG */
    esp_event_post(EMU_TEST_EVENTS, EMU_EVENT_PING, NULL, 0, portMAX_DELAY);
    esp_event_post(EMU_TEST_EVENTS, EMU_EVENT_PING, NULL, 0, portMAX_DELAY);
    esp_event_post(EMU_TEST_EVENTS, EMU_EVENT_PONG, NULL, 0, portMAX_DELAY);

    if (xSemaphoreTake(g_evt_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        FAIL_MSG(TN, "PONG not delivered in 500ms");
        goto fail;
    }
    /* Drain any pending callbacks */
    vTaskDelay(pdMS_TO_TICKS(20));

    if (g_evt_specific != 2) { FAIL_MSG(TN, "specific=%d (want 2)", g_evt_specific); goto fail; }
    if (g_evt_any_id   != 3) { FAIL_MSG(TN, "any_id=%d (want 3)",  g_evt_any_id);   goto fail; }
    if (g_evt_any_base != 3) { FAIL_MSG(TN, "any_base=%d (want 3)",g_evt_any_base); goto fail; }

    /* Unregister specific, post another PING, count must NOT increment */
    esp_event_handler_unregister(EMU_TEST_EVENTS, EMU_EVENT_PING, evt_specific_handler);
    esp_event_post(EMU_TEST_EVENTS, EMU_EVENT_PING, NULL, 0, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (g_evt_specific != 2) { FAIL_MSG(TN, "specific incr after unregister: %d", g_evt_specific); goto fail; }
    if (g_evt_any_id   != 4) { FAIL_MSG(TN, "any_id=%d after extra (want 4)",  g_evt_any_id);   goto fail; }

    esp_event_handler_unregister(EMU_TEST_EVENTS, ESP_EVENT_ANY_ID, evt_any_id_handler);
    esp_event_handler_unregister(ESP_EVENT_ANY_BASE, ESP_EVENT_ANY_ID, evt_any_base_handler);
    vSemaphoreDelete(g_evt_done);
    return true;
fail:
    esp_event_handler_unregister(EMU_TEST_EVENTS, EMU_EVENT_PING, evt_specific_handler);
    esp_event_handler_unregister(EMU_TEST_EVENTS, ESP_EVENT_ANY_ID, evt_any_id_handler);
    esp_event_handler_unregister(ESP_EVENT_ANY_BASE, ESP_EVENT_ANY_ID, evt_any_base_handler);
    vSemaphoreDelete(g_evt_done);
    return false;
}
