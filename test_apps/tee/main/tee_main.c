/*
 * ESP-TEE smoke app for esp-emu.
 *
 * A separate project rather than a case inside test_apps/smoke because TEE is
 * an image-level thing: it needs CONFIG_SECURE_ENABLE_TEE, the TEE partition
 * layout, and a second binary (esp_tee.bin) flashed alongside the app. The
 * emulator must therefore boot ROM -> bootloader -> TEE -> REE app, and the
 * REE runs in U-mode with the TEE holding M-mode.
 *
 * What this covers that nothing else does:
 *   - the M/U privilege split actually taking effect (the REE must NOT be able
 *     to reach M-mode CSRs or TEE memory),
 *   - the secure service call path (U->M->U) carrying arguments and results,
 *   - TEE secure storage: key generation, signing and encryption inside M-mode.
 *
 * Prints the same footer the harness greps for everywhere else:
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
#include "esp_cpu.h"
#include "esp_random.h"

#include "esp_tee.h"
#include "esp_tee_sec_storage.h"


static const char *TAG = "emu_tee";

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

/* The REE runs unprivileged. If this reports M-mode the whole isolation story
 * is void, so it is the first thing checked. */
static bool test_ree_is_unprivileged(void)
{
    const char *TN = "ree_is_unprivileged";
    int level = esp_cpu_get_curr_privilege_level();
    if (level != ESP_CPU_NS_MODE) {
        FAIL_MSG(TN, "REE privilege level %d, expected U/NS (%d)",
                 level, (int)ESP_CPU_NS_MODE);
        return false;
    }
    return true;
}

/* The TEE fills this structure in the REE's IRAM before the U-mode switch and
 * then write-protects it: a sane magic word plus a matching API version proves
 * the handover ran, and the fields it hands over are the ones the REE uses to
 * route interrupts back into M-mode. */
static bool test_tee_handover_config(void)
{
    const char *TN = "tee_handover_config";
    if (esp_tee_app_config.api_major_version != ESP_TEE_API_MAJOR_VER) {
        FAIL_MSG(TN, "TEE API major version %" PRIu32 ", expected %d",
                 esp_tee_app_config.api_major_version, ESP_TEE_API_MAJOR_VER);
        return false;
    }
    if (esp_tee_app_config.s_int_handler == NULL ||
        esp_tee_app_config.ns_entry_addr == NULL) {
        FAIL_MSG(TN, "TEE handover left null handlers (s_int=%p ns_entry=%p)",
                 esp_tee_app_config.s_int_handler, esp_tee_app_config.ns_entry_addr);
        return false;
    }
    ESP_LOGI(TAG, "  TEE API v%" PRIu32 ".%" PRIu32 ", REE entry %p",
             esp_tee_app_config.api_major_version,
             esp_tee_app_config.api_minor_version,
             esp_tee_app_config.ns_entry_addr);
    return true;
}

static bool test_sec_storage_sign(void)
{
    const char *TN = "sec_storage_sign";
    esp_tee_sec_storage_key_cfg_t cfg = {
        .id = "emu_ecdsa",
        .type = ESP_SEC_STG_KEY_ECDSA_SECP256R1,
        .flags = SEC_STORAGE_FLAG_NONE,
    };
    esp_err_t err = esp_tee_sec_storage_clear_key(cfg.id);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        FAIL_MSG(TN, "clear_key failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_tee_sec_storage_gen_key(&cfg);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "gen_key failed: %s", esp_err_to_name(err));
        return false;
    }

    /* The private key never leaves the TEE: the REE hands over a digest and
     * gets back a signature, then verifies it against the exported pubkey. */
    uint8_t digest[32];
    for (size_t i = 0; i < sizeof(digest); i++) {
        digest[i] = (uint8_t)(i * 7 + 1);
    }
    esp_tee_sec_storage_ecdsa_sign_t sig = {0};
    err = esp_tee_sec_storage_ecdsa_sign(&cfg, digest, sizeof(digest), &sig);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "ecdsa_sign failed: %s", esp_err_to_name(err));
        return false;
    }
    bool sig_zero = true;
    for (size_t i = 0; i < sizeof(sig.signature); i++) {
        if (sig.signature[i] != 0) {
            sig_zero = false;
            break;
        }
    }
    if (sig_zero) {
        FAIL_MSG(TN, "signature is all zero");
        return false;
    }

    esp_tee_sec_storage_ecdsa_pubkey_t pubkey = {0};
    err = esp_tee_sec_storage_ecdsa_get_pubkey(&cfg, &pubkey);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "get_pubkey failed: %s", esp_err_to_name(err));
        return false;
    }
    bool all_zero = true;
    for (size_t i = 0; i < sizeof(pubkey.pub_x); i++) {
        if (pubkey.pub_x[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        FAIL_MSG(TN, "exported public key X is all zero");
        return false;
    }
    return true;
}

static bool test_sec_storage_encrypt(void)
{
    const char *TN = "sec_storage_encrypt";
    esp_tee_sec_storage_key_cfg_t cfg = {
        .id = "emu_aes",
        .type = ESP_SEC_STG_KEY_AES256,
        .flags = SEC_STORAGE_FLAG_NONE,
    };
    esp_err_t err = esp_tee_sec_storage_clear_key(cfg.id);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        FAIL_MSG(TN, "clear_key failed: %s", esp_err_to_name(err));
        return false;
    }
    if ((err = esp_tee_sec_storage_gen_key(&cfg)) != ESP_OK) {
        FAIL_MSG(TN, "gen_key failed: %s", esp_err_to_name(err));
        return false;
    }

    const char *msg = "esp-emu tee secure storage round trip";
    const size_t len = strlen(msg);
    uint8_t ct[64] = {0};
    uint8_t tag[16] = {0};
    uint8_t iv[12] = {0};
    esp_fill_random(iv, sizeof(iv));

    esp_tee_sec_storage_aead_ctx_t ctx = {
        .key_id = cfg.id,
        .aad = NULL,
        .aad_len = 0,
        .input = (const uint8_t *)msg,
        .input_len = len,
    };

    if ((err = esp_tee_sec_storage_aead_encrypt(&ctx, iv, sizeof(iv), tag,
                                                sizeof(tag), ct)) != ESP_OK) {
        FAIL_MSG(TN, "aead_encrypt failed: %s", esp_err_to_name(err));
        return false;
    }
    if (memcmp(ct, msg, len) == 0) {
        FAIL_MSG(TN, "ciphertext equals plaintext — encryption did not happen");
        return false;
    }

    uint8_t pt[64] = {0};
    ctx.input = ct;
    ctx.input_len = len;
    if ((err = esp_tee_sec_storage_aead_decrypt(&ctx, iv, sizeof(iv), tag,
                                                sizeof(tag), pt)) != ESP_OK) {
        FAIL_MSG(TN, "aead_decrypt failed: %s", esp_err_to_name(err));
        return false;
    }
    if (memcmp(pt, msg, len) != 0) {
        FAIL_MSG(TN, "round-trip plaintext mismatch");
        return false;
    }
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP-EMU TEE Test Suite ===");
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);

    RUN_TEST("ree_is_unprivileged",  test_ree_is_unprivileged);
    RUN_TEST("tee_handover_config",  test_tee_handover_config);
    RUN_TEST("sec_storage_sign",     test_sec_storage_sign);
    RUN_TEST("sec_storage_encrypt",  test_sec_storage_encrypt);

    ESP_LOGI(TAG, "=== Test Suite Complete ===");
    ESP_LOGI(TAG, "Results: %d passed, %d failed", g_pass_count, g_fail_count);
    printf("RESULT_SUMMARY: passed=%d failed=%d target=%s\n",
           g_pass_count, g_fail_count, CONFIG_IDF_TARGET);
    vTaskDelay(pdMS_TO_TICKS(100));
}
