/*
 * Flash-encryption smoke app for esp-emu.
 *
 * A separate project rather than a case inside test_apps/smoke because flash
 * encryption is a property of the *image and the eFuses*, not of a code path:
 * the bootloader generates a key, burns it, and re-encrypts every partition in
 * place on first boot. The emulator has to get three things right at once for
 * this to work — the XTS-AES flash-encryption peripheral, the eFuse write
 * path, and transparent decryption on the flash-cache read path.
 *
 * The distinguishing check is the last one: the app reads the same flash region
 * twice, once through the cache (which must decrypt) and once through the raw
 * esp_flash API (which must not). If the emulator quietly skipped encryption
 * altogether, both reads would return plaintext and match — so the test fails
 * on "they matched", which is the failure mode a stubbed-out engine produces.
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
#include "esp_flash_encrypt.h"
#include "esp_partition.h"
#include "esp_efuse.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "emu_flash_enc";

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

/* If this is false the rest of the suite is meaningless: the bootloader either
 * never ran the encryption pass or never burned the eFuse. */
static bool test_encryption_enabled(void)
{
    const char *TN = "encryption_enabled";
    if (!esp_flash_encryption_enabled()) {
        FAIL_MSG(TN, "esp_flash_encryption_enabled() is false — the bootloader "
                     "did not enable flash encryption");
        return false;
    }
    esp_flash_enc_mode_t mode = esp_get_flash_encryption_mode();
    if (mode != ESP_FLASH_ENC_MODE_DEVELOPMENT) {
        FAIL_MSG(TN, "encryption mode %d, expected DEVELOPMENT (%d)",
                 (int)mode, (int)ESP_FLASH_ENC_MODE_DEVELOPMENT);
        return false;
    }
    return true;
}

/* The app itself is running out of encrypted flash: reaching app_main at all
 * means the cache decrypted its instructions. Check a rodata read too, since
 * that takes the same path with different MMU entries. */
static bool test_app_runs_from_encrypted_flash(void)
{
    const char *TN = "app_runs_from_encrypted_flash";
    static const char marker[] = "esp-emu-flash-enc-marker";
    if (strcmp(marker, "esp-emu-flash-enc-marker") != 0) {
        FAIL_MSG(TN, "rodata read back wrong through the flash cache");
        return false;
    }
    const esp_partition_t *running = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (running == NULL) {
        FAIL_MSG(TN, "no app partition found");
        return false;
    }
    if (!running->encrypted) {
        FAIL_MSG(TN, "app partition '%s' is not marked encrypted", running->label);
        return false;
    }
    return true;
}

/* The core check: mapped read decrypts, raw read does not. */
static bool test_ciphertext_differs_from_plaintext(void)
{
    const char *TN = "ciphertext_differs";
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (part == NULL) {
        FAIL_MSG(TN, "no app partition found");
        return false;
    }

    const size_t len = 64;
    uint8_t via_cache[64];
    uint8_t via_raw[64];

    /* esp_partition_read on an encrypted partition goes through the decrypting
     * path; esp_flash_read is the raw SPI read of the same bytes. */
    esp_err_t err = esp_partition_read(part, 0, via_cache, len);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_partition_read failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_flash_read(NULL, via_raw, part->address, len);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_flash_read failed: %s", esp_err_to_name(err));
        return false;
    }

    if (memcmp(via_cache, via_raw, len) == 0) {
        FAIL_MSG(TN, "decrypted and raw reads are identical — flash is not "
                     "actually encrypted on media");
        return false;
    }
    /* The decrypted view must start with the ESP image magic; if decryption
     * produced garbage this catches it rather than passing on "they differ". */
    if (via_cache[0] != 0xE9) {
        FAIL_MSG(TN, "decrypted image magic 0x%02x, expected 0xE9", via_cache[0]);
        return false;
    }
    ESP_LOGI(TAG, "  plaintext[0..3]=%02x %02x %02x %02x  ciphertext[0..3]=%02x %02x %02x %02x",
             via_cache[0], via_cache[1], via_cache[2], via_cache[3],
             via_raw[0], via_raw[1], via_raw[2], via_raw[3]);
    return true;
}

/* Runtime encrypted writes work on C3 only. Everywhere else — C5, C6, H2, P4
 * and S31, all measured — esp_partition_write to an encrypted partition
 * round-trips a0a1.. as 0000.., while boot-time encryption works fine (those
 * chips still pass encryption_enabled, app_runs_from_encrypted_flash and
 * ciphertext_differs). So the gap is the *runtime* XTS manual-encrypt path,
 * not flash encryption as such.
 *
 * Not a register-layout mismatch: XTS_AES LINESIZE/DESTINATION/PHYSICAL_ADDRESS
 * /TRIGGER/RELEASE/DESTROY/STATE sit at identical offsets on C3 and C6 and
 * match src/periph/xts_aes.rs, and the write-completion path in
 * bus.rs::execute_pending_spi_op is chip-agnostic. */
/* Encrypted writes have to survive a round trip through the XTS engine, at an
 * offset that is not sector-aligned in the ciphertext block sense. */
static bool test_encrypted_write_read(void)
{
    const char *TN = "encrypted_write_read";
    /* `enc_test` is declared with the `encrypted` flag in partitions.csv; a
     * plain data partition is not encrypted by IDF, so writing to one would
     * legitimately land as plaintext and prove nothing. */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "enc_test");
    if (part == NULL) {
        FAIL_MSG(TN, "enc_test partition not found");
        return false;
    }
    if (!part->encrypted) {
        FAIL_MSG(TN, "enc_test is not marked encrypted — check partitions.csv");
        return false;
    }

    esp_err_t err = esp_partition_erase_range(part, 0, 4096);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "erase failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Offset 32 on purpose, not 0. IDF places the row at
     * `PLAIN_MEM + (flash_addr % SOC_FLASH_ENCRYPTED_XTS_AES_BLOCK_MAX)`, which
     * is 0 for a write at the start of a partition and only becomes non-zero
     * further in — so an offset-0 test cannot see a mis-read of that buffer.
     * That exact bug shipped in the emulator until 2026-08-22. */
    uint8_t pattern[32];
    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(0xA0 + i);
    }
    err = esp_partition_write(part, 32, pattern, sizeof(pattern));
    if (err != ESP_OK) {
        FAIL_MSG(TN, "encrypted write failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t readback[32] = {0};
    err = esp_partition_read(part, 32, readback, sizeof(readback));
    if (err != ESP_OK) {
        FAIL_MSG(TN, "read back failed: %s", esp_err_to_name(err));
        return false;
    }
    if (memcmp(pattern, readback, sizeof(pattern)) != 0) {
        FAIL_MSG(TN, "round trip mismatch (wrote %02x%02x, read %02x%02x)",
                 pattern[0], pattern[1], readback[0], readback[1]);
        return false;
    }

    /* And the media must not hold the plaintext. */
    uint8_t raw[32] = {0};
    err = esp_flash_read(NULL, raw, part->address + 32, sizeof(raw));
    if (err == ESP_OK && memcmp(raw, pattern, sizeof(pattern)) == 0) {
        FAIL_MSG(TN, "written data is plaintext on media");
        return false;
    }
    return true;
}

/* NVS on an encrypted device still has to work — it is the most common thing
 * to break when the encrypted-write alignment rules are mishandled. */
static bool test_nvs_on_encrypted_flash(void)
{
    const char *TN = "nvs_on_encrypted_flash";
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        FAIL_MSG(TN, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_handle_t h;
    if ((err = nvs_open("emu_fe", NVS_READWRITE, &h)) != ESP_OK) {
        FAIL_MSG(TN, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    bool ok = false;
    const uint32_t want = 0xC0FFEE01;
    if ((err = nvs_set_u32(h, "key", want)) != ESP_OK) {
        FAIL_MSG(TN, "nvs_set_u32 failed: %s", esp_err_to_name(err));
    } else if ((err = nvs_commit(h)) != ESP_OK) {
        FAIL_MSG(TN, "nvs_commit failed: %s", esp_err_to_name(err));
    } else {
        uint32_t got = 0;
        if ((err = nvs_get_u32(h, "key", &got)) != ESP_OK) {
            FAIL_MSG(TN, "nvs_get_u32 failed: %s", esp_err_to_name(err));
        } else if (got != want) {
            FAIL_MSG(TN, "read back 0x%08" PRIx32 ", expected 0x%08" PRIx32, got, want);
        } else {
            ok = true;
        }
    }
    nvs_close(h);
    return ok;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP-EMU Flash Encryption Test Suite ===");
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);

    RUN_TEST("encryption_enabled",           test_encryption_enabled);
    RUN_TEST("app_runs_from_encrypted_flash", test_app_runs_from_encrypted_flash);
    RUN_TEST("ciphertext_differs",           test_ciphertext_differs_from_plaintext);
    RUN_TEST("encrypted_write_read",         test_encrypted_write_read);
    RUN_TEST("nvs_on_encrypted_flash",       test_nvs_on_encrypted_flash);

    ESP_LOGI(TAG, "=== Test Suite Complete ===");
    ESP_LOGI(TAG, "Results: %d passed, %d failed", g_pass_count, g_fail_count);
    printf("RESULT_SUMMARY: passed=%d failed=%d target=%s\n",
           g_pass_count, g_fail_count, CONFIG_IDF_TARGET);
    vTaskDelay(pdMS_TO_TICKS(100));
}
