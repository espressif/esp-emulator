/*
 * Hardware-accelerated crypto through PSA, plus the RNG.
 */
#include "test_smoke.h"

/* --------------- crypto ---------------
 *
 * These exercise the HW accelerators through mbedtls' PSA front-end: the SHA
 * case deliberately hashes >4 KiB so the driver takes the DMA path (a
 * descriptor-level bug there is otherwise silent — it just hashes the wrong
 * bytes and still returns success).
 */

/* SHA-256 of 8192 bytes of 0xA5, from:
 *   python3 -c "import hashlib;print(hashlib.sha256(b'\xa5'*8192).hexdigest())"
 */
static const uint8_t k_sha256_a5_8k[32] = {
    0x2e, 0xf1, 0x44, 0x4b, 0xc9, 0x50, 0x05, 0x0c, 0x92, 0xf3, 0x73, 0xcd,
    0x2f, 0x54, 0x42, 0x02, 0x2a, 0xf9, 0x8a, 0xa9, 0x00, 0xae, 0xfd, 0x82,
    0xc7, 0x49, 0xcf, 0xf9, 0x3d, 0x4c, 0x00, 0x37,
};

/* AES-128-ECB known answer (FIPS-197 C.1): key/plaintext 00112233..ff. */
static const uint8_t k_aes128_key[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};
static const uint8_t k_aes128_pt[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};
static const uint8_t k_aes128_ct[16] = {
    0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
    0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
};

static bool s_psa_ready;

static bool crypto_init(const char *tn)
{
    if (s_psa_ready) {
        return true;
    }
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        FAIL_MSG(tn, "psa_crypto_init failed: %d", (int)st);
        return false;
    }
    s_psa_ready = true;
    return true;
}

bool test_crypto_sha256_dma(void)
{
    const char *TN = "crypto_sha256_dma";
    if (!crypto_init(TN)) {
        return false;
    }

    const size_t len = 8192;
    uint8_t *buf = malloc(len);
    if (!buf) {
        FAIL_MSG(TN, "malloc(%u) failed", (unsigned)len);
        return false;
    }
    memset(buf, 0xA5, len);

    uint8_t digest[32];
    size_t out_len = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, buf, len,
                                       digest, sizeof(digest), &out_len);
    free(buf);
    if (st != PSA_SUCCESS) {
        FAIL_MSG(TN, "psa_hash_compute failed: %d", (int)st);
        return false;
    }
    if (out_len != sizeof(digest) || memcmp(digest, k_sha256_a5_8k, sizeof(digest)) != 0) {
        FAIL_MSG(TN, "digest mismatch (first byte %02x, expected %02x)",
                 digest[0], k_sha256_a5_8k[0]);
        return false;
    }
    return true;
}

bool test_crypto_aes128(void)
{
    const char *TN = "crypto_aes128";
    if (!crypto_init(TN)) {
        return false;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);

    psa_key_id_t key = 0;
    psa_status_t st = psa_import_key(&attr, k_aes128_key, sizeof(k_aes128_key), &key);
    if (st != PSA_SUCCESS) {
        FAIL_MSG(TN, "psa_import_key failed: %d", (int)st);
        return false;
    }

    bool ok = false;
    uint8_t ct[32];
    size_t ct_len = 0;
    st = psa_cipher_encrypt(key, PSA_ALG_ECB_NO_PADDING, k_aes128_pt, sizeof(k_aes128_pt),
                            ct, sizeof(ct), &ct_len);
    if (st != PSA_SUCCESS) {
        FAIL_MSG(TN, "psa_cipher_encrypt failed: %d", (int)st);
    } else if (ct_len != sizeof(k_aes128_ct) ||
               memcmp(ct, k_aes128_ct, sizeof(k_aes128_ct)) != 0) {
        FAIL_MSG(TN, "ciphertext mismatch (got %02x%02x, expected %02x%02x)",
                 ct[0], ct[1], k_aes128_ct[0], k_aes128_ct[1]);
    } else {
        uint8_t pt[32];
        size_t pt_len = 0;
        st = psa_cipher_decrypt(key, PSA_ALG_ECB_NO_PADDING, ct, ct_len,
                                pt, sizeof(pt), &pt_len);
        if (st != PSA_SUCCESS) {
            FAIL_MSG(TN, "psa_cipher_decrypt failed: %d", (int)st);
        } else if (pt_len != sizeof(k_aes128_pt) ||
                   memcmp(pt, k_aes128_pt, sizeof(k_aes128_pt)) != 0) {
            FAIL_MSG(TN, "round-trip plaintext mismatch");
        } else {
            ok = true;
        }
    }
    psa_destroy_key(key);
    return ok;
}

bool test_crypto_ecdsa_p256(void)
{
    const char *TN = "crypto_ecdsa_p256";
    if (!crypto_init(TN)) {
        return false;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);

    psa_key_id_t key = 0;
    psa_status_t st = psa_generate_key(&attr, &key);
    if (st != PSA_SUCCESS) {
        FAIL_MSG(TN, "psa_generate_key failed: %d", (int)st);
        return false;
    }

    /* Sign a fixed digest, then verify it — and verify that a flipped bit is
     * rejected, so a stubbed-out verify can't pass this case. */
    uint8_t hash[32];
    for (size_t i = 0; i < sizeof(hash); i++) {
        hash[i] = (uint8_t)i;
    }
    uint8_t sig[PSA_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0;
    bool ok = false;
    st = psa_sign_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash),
                       sig, sizeof(sig), &sig_len);
    if (st != PSA_SUCCESS) {
        FAIL_MSG(TN, "psa_sign_hash failed: %d", (int)st);
    } else if ((st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash,
                                     sizeof(hash), sig, sig_len)) != PSA_SUCCESS) {
        FAIL_MSG(TN, "psa_verify_hash rejected a good signature: %d", (int)st);
    } else {
        hash[0] ^= 0xFF;
        st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash),
                             sig, sig_len);
        if (st == PSA_SUCCESS) {
            FAIL_MSG(TN, "psa_verify_hash accepted a tampered digest");
        } else {
            ok = true;
        }
    }
    psa_destroy_key(key);
    return ok;
}

bool test_crypto_hmac_sha256(void)
{
    const char *TN = "crypto_hmac_sha256";
    if (!crypto_init(TN)) {
        return false;
    }

    /* RFC 4231 test case 1: key = 20x 0x0b, data = "Hi There". */
    const uint8_t key_bytes[20] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    };
    static const uint8_t expected[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf,
        0xce, 0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83,
        0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);

    psa_key_id_t key = 0;
    psa_status_t st = psa_import_key(&attr, key_bytes, sizeof(key_bytes), &key);
    if (st != PSA_SUCCESS) {
        FAIL_MSG(TN, "psa_import_key failed: %d", (int)st);
        return false;
    }

    uint8_t mac[32];
    size_t mac_len = 0;
    bool ok = false;
    st = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                         (const uint8_t *)"Hi There", 8, mac, sizeof(mac), &mac_len);
    if (st != PSA_SUCCESS) {
        FAIL_MSG(TN, "psa_mac_compute failed: %d", (int)st);
    } else if (mac_len != sizeof(expected) || memcmp(mac, expected, sizeof(expected)) != 0) {
        FAIL_MSG(TN, "HMAC mismatch (got %02x%02x, expected %02x%02x)",
                 mac[0], mac[1], expected[0], expected[1]);
    } else {
        ok = true;
    }
    psa_destroy_key(key);
    return ok;
}

bool test_esp_random(void)
{
    const char *TN = "esp_random";
    /* The emulator's TRNG must not be a constant: the bootloader spins on some
     * chips if it is, and a stuck RNG silently weakens everything above it. */
    uint32_t vals[8];
    for (size_t i = 0; i < 8; i++) {
        vals[i] = esp_random();
    }
    bool all_same = true;
    for (size_t i = 1; i < 8; i++) {
        if (vals[i] != vals[0]) {
            all_same = false;
            break;
        }
    }
    if (all_same) {
        FAIL_MSG(TN, "esp_random() returned %08" PRIx32 " eight times", vals[0]);
        return false;
    }

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    esp_fill_random(buf, sizeof(buf));
    size_t zeros = 0;
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] == 0) {
            zeros++;
        }
    }
    if (zeros > sizeof(buf) / 2) {
        FAIL_MSG(TN, "esp_fill_random left %u/%u bytes zero", (unsigned)zeros,
                 (unsigned)sizeof(buf));
        return false;
    }
    return true;
}

/* --------------- hardware ECDSA ---------------
 *
 * Ported from ESP-IDF's `esp_hal_security/test_apps/crypto/main/ecdsa/
 * test_ecdsa.c`, reusing its vectors and key. Upstream needs those burned by
 * hand with espefuse, so this burns its own — which requires
 * CONFIG_EFUSE_VIRTUAL=n, since a virtual key never reaches the hardware block.
 *
 * Sign and export both consume the eFuse key, so they agree with each other
 * even when it is read in the wrong byte order; only the off-device public
 * point from `ecdsa_params.h` catches that.
 */
#if SOC_ECDSA_SUPPORTED
#include "hal/ecdsa_hal.h"
#include "esp_crypto_periph_clk.h"
#include "esp_efuse.h"
#include "esp_efuse_chip.h"
#include "hal/ecdsa_ll.h"

#define ECDSA_TEST_EFUSE_BLK    (EFUSE_BLK_KEY0 + 1)

/* `ecdsa256_priv_key.pem`'s scalar little-endian: what `espefuse burn-key`
 * writes, since it reverses every ECDSA_KEY/XTS_AES purpose.
 * `esp_efuse_write_key` below does not reverse, so it takes the block form. */
static const uint8_t k_ecdsa256_key_blk[32] = {
    0xf3, 0x47, 0x2b, 0x4c, 0xcf, 0xc7, 0xcb, 0x20,
    0x30, 0x73, 0x10, 0xa3, 0x7f, 0x6d, 0x7e, 0x24,
    0x3d, 0x36, 0x00, 0x3a, 0x48, 0x4b, 0xac, 0xd1,
    0x2e, 0x36, 0x15, 0x3e, 0x32, 0x63, 0x25, 0x11,
};

/* Curve-specific purposes exist on some targets and alias ECDSA_KEY on others. */
#if SOC_ECDSA_SUPPORT_CURVE_SPECIFIC_KEY_PURPOSES
#define ECDSA_TEST_PURPOSE  ESP_EFUSE_KEY_PURPOSE_ECDSA_KEY_P256
#else
#define ECDSA_TEST_PURPOSE  ESP_EFUSE_KEY_PURPOSE_ECDSA_KEY
#endif

/* eFuse bits are write-once and the app reboots on purpose (reset-reason
 * phases), so a re-burn must be a no-op rather than a failure. */
static bool ecdsa_burn_test_key(const char *tn)
{
    if (esp_efuse_get_key_purpose(ECDSA_TEST_EFUSE_BLK) == ECDSA_TEST_PURPOSE) {
        return true;
    }
    esp_err_t err = esp_efuse_write_key(ECDSA_TEST_EFUSE_BLK, ECDSA_TEST_PURPOSE,
                                        k_ecdsa256_key_blk, sizeof(k_ecdsa256_key_blk));
    if (err != ESP_OK) {
        FAIL_MSG(tn, "esp_efuse_write_key failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* All little-endian, as the HAL reads and writes them. */
static const uint8_t k_ecdsa256_r[32] = {
    0xf7, 0xc2, 0x33, 0x2f, 0xf2, 0x9e, 0xa1, 0x2f,
    0x83, 0x9f, 0x8b, 0x68, 0xc4, 0x15, 0xd0, 0x6a,
    0x1b, 0xd1, 0x25, 0x0c, 0x7e, 0x66, 0x98, 0xeb,
    0x0e, 0x49, 0x4d, 0xf4, 0xf2, 0x35, 0x58, 0xbf,
};
static const uint8_t k_ecdsa256_s[32] = {
    0x79, 0x7c, 0x91, 0x4b, 0xe2, 0x73, 0x0d, 0x7e,
    0x7a, 0xc1, 0xb1, 0xe4, 0xfa, 0xf9, 0xb4, 0xdf,
    0x29, 0xe7, 0x5f, 0xbb, 0xdf, 0x43, 0xfe, 0xd0,
    0xc1, 0xcf, 0xe4, 0xaa, 0x5f, 0xb1, 0x63, 0x25,
};
static const uint8_t k_ecdsa256_pub_x[32] = {
    0xb9, 0x7f, 0x76, 0x8b, 0x63, 0x0f, 0x8c, 0x7c,
    0xdd, 0x67, 0x00, 0x2a, 0x96, 0x56, 0x5c, 0x84,
    0xfa, 0xc7, 0xbf, 0x89, 0xb1, 0x51, 0x2f, 0x13,
    0x3c, 0x54, 0x9b, 0x20, 0x60, 0x52, 0x8f, 0xa2,
};
static const uint8_t k_ecdsa256_pub_y[32] = {
    0xd5, 0x05, 0x74, 0x01, 0x47, 0xf2, 0x81, 0x1e,
    0x1c, 0xc3, 0x5c, 0xf7, 0x8c, 0x49, 0xad, 0x7e,
    0xac, 0xde, 0x7c, 0x0d, 0x72, 0x04, 0x53, 0xc4,
    0x0a, 0x59, 0x9b, 0x5a, 0x5b, 0x87, 0x4c, 0xf6,
};
/* Big-endian digest; the HAL wants it little-endian (`ecc_be_to_le`). */
static const uint8_t k_ecdsa_sha_be[48] = {
    0x98, 0xca, 0xea, 0x85, 0x7b, 0x03, 0x5e, 0xc0,
    0xe3, 0xc3, 0x39, 0x29, 0xef, 0xf1, 0xf1, 0x25,
    0x00, 0x19, 0xe7, 0x11, 0xc3, 0x3d, 0x84, 0x42,
    0x38, 0x79, 0x10, 0xef, 0xb2, 0x9b, 0xd2, 0x63,
    0xed, 0xfe, 0x04, 0xce, 0x66, 0x89, 0xd0, 0xa4,
    0xb2, 0x60, 0xb2, 0x38, 0x93, 0xa6, 0x27, 0x14,
};

static void ecdsa_be_to_le(const uint8_t *be, uint8_t *le, uint8_t len)
{
    memset(le, 0, len);
    for (int i = 0; i < len; i++) {
        le[i] = be[len - i - 1];
    }
}

static int ecdsa_hw_verify(const uint8_t *r, const uint8_t *s,
                           const uint8_t *qx, const uint8_t *qy)
{
    uint8_t sha_le[32];
    ecdsa_be_to_le(k_ecdsa_sha_be, sha_le, 32);
    ecdsa_hal_config_t conf = {
        .mode = ECDSA_MODE_SIGN_VERIFY,
        .curve = ECDSA_CURVE_SECP256R1,
        .sha_mode = ECDSA_Z_USER_PROVIDED,
    };
    esp_crypto_ecdsa_enable_periph_clk(true);
    int ret = ecdsa_hal_verify_signature(&conf, sha_le, (uint8_t *)r, (uint8_t *)s,
                                         (uint8_t *)qx, (uint8_t *)qy, 32);
    esp_crypto_ecdsa_enable_periph_clk(false);
    return ret;
}

bool test_crypto_ecdsa_hw_verify(void)
{
    const char *TN = "crypto_ecdsa_hw_verify";
    if (ecdsa_hw_verify(k_ecdsa256_r, k_ecdsa256_s,
                        k_ecdsa256_pub_x, k_ecdsa256_pub_y) != 0) {
        FAIL_MSG(TN, "peripheral rejected a known-good P-256 signature");
        return false;
    }
    /* Corrupt R — a stubbed-out verify that always returns 0 must not pass. */
    uint8_t bad_r[32];
    memcpy(bad_r, k_ecdsa256_r, sizeof(bad_r));
    bad_r[0] ^= 0xFF;
    if (ecdsa_hw_verify(bad_r, k_ecdsa256_s,
                        k_ecdsa256_pub_x, k_ecdsa256_pub_y) == 0) {
        FAIL_MSG(TN, "peripheral accepted a corrupted signature");
        return false;
    }
    return true;
}

#if SOC_ECDSA_SUPPORT_EXPORT_PUBKEY
/* Export takes no z, so `configure_ecdsa_periph` skips `ecdsa_ll_set_z_mode`
 * and CONF.SOFTWARE_SET_Z keeps whatever ran last — 0 after reset. Treating
 * that bit as mandatory hangs here rather than failing (GH #7), which the
 * harness reports as a timeout. Cleared explicitly below because the LL
 * read-modify-writes CONF: any earlier sign or verify leaves it set, making
 * this a warm export that passes either way. */
bool test_crypto_ecdsa_export_pubkey(void)
{
    const char *TN = "crypto_ecdsa_export_pubkey";
    if (!ecdsa_burn_test_key(TN)) {
        return false;
    }
    uint8_t qx[32] = {0};
    uint8_t qy[32] = {0};
    ecdsa_hal_config_t conf = {
        .mode = ECDSA_MODE_EXPORT_PUBKEY,
        .curve = ECDSA_CURVE_SECP256R1,
        .use_km_key = 0,
        .efuse_key_blk = ECDSA_TEST_EFUSE_BLK,
    };
    esp_crypto_ecdsa_enable_periph_clk(true);
    ecdsa_ll_set_z_mode(ECDSA_Z_USE_SHA_PERI); /* clear SOFTWARE_SET_Z */
    ecdsa_hal_export_pubkey(&conf, qx, qy, 32);
    bool ok = ecdsa_hal_get_operation_result();
    esp_crypto_ecdsa_enable_periph_clk(false);

    if (!ok) {
        FAIL_MSG(TN, "export reported failure");
        return false;
    }
    /* Against the burned key's real public point, so a byte-reversed read of
     * the eFuse block fails here even though it would still self-verify. */
    if (memcmp(qx, k_ecdsa256_pub_x, 32) != 0 ||
        memcmp(qy, k_ecdsa256_pub_y, 32) != 0) {
        FAIL_MSG(TN, "exported pubkey != the burned key's public point");
        return false;
    }
    return true;
}
#endif /* SOC_ECDSA_SUPPORT_EXPORT_PUBKEY */

bool test_crypto_ecdsa_sign(void)
{
    const char *TN = "crypto_ecdsa_sign";
    if (!ecdsa_burn_test_key(TN)) {
        return false;
    }
    uint8_t sha_le[32];
    uint8_t r[32] = {0};
    uint8_t s[32] = {0};
    ecdsa_be_to_le(k_ecdsa_sha_be, sha_le, 32);

    ecdsa_hal_config_t conf = {
        .mode = ECDSA_MODE_SIGN_GEN,
        .curve = ECDSA_CURVE_SECP256R1,
        .sha_mode = ECDSA_Z_USER_PROVIDED,
        .sign_type = ECDSA_K_TYPE_TRNG,
        .use_km_key = 0,
        .efuse_key_blk = ECDSA_TEST_EFUSE_BLK,
    };
    esp_crypto_ecdsa_enable_periph_clk(true);
    ecdsa_hal_gen_signature(&conf, sha_le, r, s, 32);
    bool ok = ecdsa_hal_get_operation_result();
    esp_crypto_ecdsa_enable_periph_clk(false);

    if (!ok) {
        FAIL_MSG(TN, "sign reported failure");
        return false;
    }
    /* Verify the fresh signature against the *known* public point rather than
     * an exported one: that is what ties the signature to the burned key. */
    if (ecdsa_hw_verify(r, s, k_ecdsa256_pub_x, k_ecdsa256_pub_y) != 0) {
        FAIL_MSG(TN, "signature does not verify against the burned key's pubkey");
        return false;
    }
    return true;
}
#endif /* SOC_ECDSA_SUPPORTED */
