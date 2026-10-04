/*
 * 32 MiB flash smoke app for esp-emu (GH #8).
 *
 * A separate project rather than a case inside test_apps/smoke because the
 * thing under test is a property of the image and the device, not of a code
 * path: the smoke app's 8 MiB layout emits no 4-byte-address opcode. See the
 * emulator README's "Flash Size" section for the mechanism and for why C6/H2
 * are absent from the suite's chip list.
 *
 * Footer, as everywhere else:
 *   RESULT_SUMMARY: passed=N failed=M target=<chip>
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_flash.h"
#include "esp_partition.h"

static const char *TAG = "emu_flash32";

#define FLASH_SIZE_BYTES (32 * 1024 * 1024)
#define SIXTEEN_MIB      (16 * 1024 * 1024)

static int g_pass_count;
static int g_fail_count;

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

/* The size the driver settles on comes from the image header by way of
 * g_rom_flashchip, cross-checked against the device's RDID capacity byte. If
 * the emulator sized its array off anything else the two disagree and
 * esp_flash_init_default_chip() would already have refused to boot. */
static bool test_detected_size(void)
{
    const char *TN = "detected_size";
    uint32_t size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &size);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_flash_get_size failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "  detected %" PRIu32 " bytes (%" PRIu32 " MiB)", size, size >> 20);
    if (size != FLASH_SIZE_BYTES) {
        FAIL_MSG(TN, "detected %" PRIu32 " bytes, expected %d", size, FLASH_SIZE_BYTES);
        return false;
    }
    return true;
}

/* The partition table has to be readable to its end — `big` runs from the
 * 16 MiB line to the top of the device. */
static const esp_partition_t *big_partition(const char *TN)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "big");
    if (p == NULL) {
        FAIL_MSG(TN, "partition 'big' not found");
        return NULL;
    }
    if (p->address + p->size <= SIXTEEN_MIB) {
        FAIL_MSG(TN, "'big' ends at 0x%08" PRIx32 ", below the 16 MiB line — "
                     "the layout under test did not get flashed",
                 p->address + p->size);
        return NULL;
    }
    return p;
}

static bool test_partition_above_16mb(void)
{
    const char *TN = "partition_above_16mb";
    const esp_partition_t *p = big_partition(TN);
    if (p == NULL) {
        return false;
    }
    ESP_LOGI(TAG, "  'big' at 0x%08" PRIx32 " size 0x%08" PRIx32, p->address, p->size);
    return true;
}

/* Erase, program and read back the last sector of the device — 0x01FFF000,
 * which no 24-bit address can name. Every one of the three goes out as a
 * *_4B opcode; an emulator that ignores those leaves the read all-0xFF. */
static bool test_roundtrip_at_top_of_device(void)
{
    const char *TN = "roundtrip_at_top_of_device";
    const esp_partition_t *p = big_partition(TN);
    if (p == NULL) {
        return false;
    }

    const size_t off = p->size - 4096;
    const uint32_t abs_addr = p->address + off;
    ESP_LOGI(TAG, "  absolute offset 0x%08" PRIx32, abs_addr);
    if (abs_addr <= 0x00FFFFFF) {
        FAIL_MSG(TN, "0x%08" PRIx32 " fits in 24 bits — not the case under test", abs_addr);
        return false;
    }

    esp_err_t err = esp_partition_erase_range(p, off, 4096);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "erase failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t wr[64], rd[64];
    for (size_t i = 0; i < sizeof(wr); i++) {
        wr[i] = (uint8_t)(i * 7 + 1);
    }
    if ((err = esp_partition_write(p, off, wr, sizeof(wr))) != ESP_OK) {
        FAIL_MSG(TN, "write failed: %s", esp_err_to_name(err));
        return false;
    }
    memset(rd, 0, sizeof(rd));
    if ((err = esp_partition_read(p, off, rd, sizeof(rd))) != ESP_OK) {
        FAIL_MSG(TN, "read failed: %s", esp_err_to_name(err));
        return false;
    }
    if (memcmp(wr, rd, sizeof(wr)) != 0) {
        FAIL_MSG(TN, "round trip mismatch (wrote %02x%02x, read %02x%02x)",
                 wr[0], wr[1], rd[0], rd[1]);
        return false;
    }

    /* The erase has to have covered the whole sector, not just the head: a
     * 24-bit-truncated address would have erased somewhere else entirely and
     * left this byte at whatever it was. */
    uint8_t tail = 0;
    if ((err = esp_partition_read(p, p->size - 1, &tail, 1)) != ESP_OK) {
        FAIL_MSG(TN, "tail read failed: %s", esp_err_to_name(err));
        return false;
    }
    if (tail != 0xFF) {
        FAIL_MSG(TN, "last byte of the device is 0x%02x, expected 0xFF after erase", tail);
        return false;
    }
    return true;
}

/* A write at the top must not alias onto the low 16 MiB — which is exactly
 * what a 24-bit address truncation does. 0x01FFF000 & 0xFFFFFF is 0x00FFF000,
 * so check that sector still reads erased. */
static bool test_no_aliasing_into_low_16mb(void)
{
    const char *TN = "no_aliasing_into_low_16mb";
    const esp_partition_t *p = big_partition(TN);
    if (p == NULL) {
        return false;
    }
    const uint32_t aliased = (p->address + p->size - 4096) & 0x00FFFFFF;

    uint8_t buf[64];
    esp_err_t err = esp_flash_read(NULL, buf, aliased, sizeof(buf));
    if (err != ESP_OK) {
        FAIL_MSG(TN, "read at 0x%08" PRIx32 " failed: %s", aliased, esp_err_to_name(err));
        return false;
    }
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0xFF) {
            FAIL_MSG(TN, "0x%08" PRIx32 " holds 0x%02x — the write at the top of "
                         "the device aliased into the low 16 MiB",
                     aliased + (uint32_t)i, buf[i]);
            return false;
        }
    }
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP-EMU 32 MiB Flash Test Suite ===");
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);

    RUN_TEST("detected_size",              test_detected_size);
    RUN_TEST("partition_above_16mb",       test_partition_above_16mb);
    RUN_TEST("roundtrip_at_top_of_device", test_roundtrip_at_top_of_device);
    RUN_TEST("no_aliasing_into_low_16mb",  test_no_aliasing_into_low_16mb);

    ESP_LOGI(TAG, "=== Test Suite Complete ===");
    ESP_LOGI(TAG, "Results: %d passed, %d failed", g_pass_count, g_fail_count);
    printf("RESULT_SUMMARY: passed=%d failed=%d target=%s\n",
           g_pass_count, g_fail_count, CONFIG_IDF_TARGET);
    vTaskDelay(pdMS_TO_TICKS(100));
}
