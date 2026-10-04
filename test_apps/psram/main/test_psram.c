/*
 * esp-emu PSRAM app — reports the size the device claims and proves the RAM
 * behind it is really there.
 *
 * The size is not the firmware's to choose on P4/S31: `esp_psram` has no size
 * Kconfig there, it reads MR2's density field off the die and believes it. So
 * what this app prints is exactly what the emulated die said, which is what
 * `--psram-size` sets (GH #9).
 *
 *   RESULT_PSRAM_SIZE: <bytes>
 *   RESULT_SUMMARY: passed=N failed=M target=<chip>
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "emu_psram";

static int g_pass;
static int g_fail;

static void check(const char *name, bool ok)
{
    printf("TEST [%s]: %s\n", name, ok ? "PASSED" : "FAILED");
    ok ? g_pass++ : g_fail++;
}

/* Write a pattern at both ends of the largest SPIRAM block the heap will give
 * and read it back. A die that over-reports its density hands out pages with
 * no store behind them, which reads back as zeros — the failure this catches
 * that a size printout alone cannot. */
static bool exercise_largest_block(void)
{
    size_t len = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (len == 0) {
        ESP_LOGE(TAG, "no SPIRAM block to allocate");
        return false;
    }
    uint32_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "allocating %u bytes of SPIRAM failed", (unsigned)len);
        return false;
    }
    ESP_LOGI(TAG, "exercising %u KB at %p", (unsigned)(len / 1024), buf);

    const size_t words = len / sizeof(uint32_t);
    bool ok = true;
    for (size_t i = 0; i < 8 && i < words; i++) {
        buf[i] = 0xA5A50000u | (uint32_t)i;
        buf[words - 1 - i] = 0x5A5A0000u | (uint32_t)i;
    }
    for (size_t i = 0; i < 8 && i < words; i++) {
        ok = ok && buf[i] == (0xA5A50000u | (uint32_t)i);
        ok = ok && buf[words - 1 - i] == (0x5A5A0000u | (uint32_t)i);
    }
    heap_caps_free(buf);
    return ok;
}

void app_main(void)
{
    size_t size = esp_psram_get_size();
    printf("RESULT_PSRAM_SIZE: %u\n", (unsigned)size);
    printf("RESULT_PSRAM_HEAP: %u\n", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    if (size == 0) {
        /* No die fitted (`--psram-size 0`). Reaching here at all is the case:
         * `SPIRAM_IGNORE_NOTFOUND` is supposed to let the app boot anyway. */
        check("psram_absent_boots", !esp_psram_is_initialized());
    } else {
        check("psram_initialized", esp_psram_is_initialized());
        /* The heap pool is the detected size minus what the driver reserves
         * for internal DMA, so it must not exceed the device. */
        check("psram_heap_within_device",
              heap_caps_get_total_size(MALLOC_CAP_SPIRAM) <= size);
        check("psram_largest_block_round_trips", exercise_largest_block());
    }

    printf("RESULT_SUMMARY: passed=%d failed=%d target=%s\n",
           g_pass, g_fail, CONFIG_IDF_TARGET);
    vTaskDelay(pdMS_TO_TICKS(100));
}
