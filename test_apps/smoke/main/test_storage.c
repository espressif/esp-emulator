/*
 * NVS, partition table, SPI flash and flash mmap.
 */
#include "test_smoke.h"

/* --------------- 1. NVS tests --------------- */

bool test_nvs(void)
{
    const char *TN = "nvs";
    esp_err_t err;

    /* NVS is already initialized by app_main bootstrap. */
    nvs_handle_t h;
    err = nvs_open("test_ns", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "nvs_open: %s", esp_err_to_name(err));
        nvs_flash_deinit();
        return false;
    }

    /* i32 round-trip */
    err = nvs_set_i32(h, "val_i32", 42);
    if (err != ESP_OK) { FAIL_MSG(TN, "set_i32: %s", esp_err_to_name(err)); goto fail; }
    err = nvs_commit(h);
    if (err != ESP_OK) { FAIL_MSG(TN, "commit: %s", esp_err_to_name(err)); goto fail; }

    int32_t v32 = 0;
    err = nvs_get_i32(h, "val_i32", &v32);
    if (err != ESP_OK) { FAIL_MSG(TN, "get_i32: %s", esp_err_to_name(err)); goto fail; }
    if (v32 != 42) { FAIL_MSG(TN, "i32 mismatch: got %d", (int)v32); goto fail; }

    /* string round-trip */
    err = nvs_set_str(h, "val_str", "hello_emu");
    if (err != ESP_OK) { FAIL_MSG(TN, "set_str: %s", esp_err_to_name(err)); goto fail; }
    err = nvs_commit(h);
    if (err != ESP_OK) { FAIL_MSG(TN, "commit str: %s", esp_err_to_name(err)); goto fail; }

    char buf[32] = {0};
    size_t len = sizeof(buf);
    err = nvs_get_str(h, "val_str", buf, &len);
    if (err != ESP_OK) { FAIL_MSG(TN, "get_str: %s", esp_err_to_name(err)); goto fail; }
    if (strcmp(buf, "hello_emu") != 0) { FAIL_MSG(TN, "str mismatch: '%s'", buf); goto fail; }

    /* blob round-trip */
    uint8_t blob_w[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    err = nvs_set_blob(h, "val_blob", blob_w, sizeof(blob_w));
    if (err != ESP_OK) { FAIL_MSG(TN, "set_blob: %s", esp_err_to_name(err)); goto fail; }
    err = nvs_commit(h);
    if (err != ESP_OK) { FAIL_MSG(TN, "commit blob: %s", esp_err_to_name(err)); goto fail; }

    uint8_t blob_r[4] = {0};
    len = sizeof(blob_r);
    err = nvs_get_blob(h, "val_blob", blob_r, &len);
    if (err != ESP_OK) { FAIL_MSG(TN, "get_blob: %s", esp_err_to_name(err)); goto fail; }
    if (memcmp(blob_w, blob_r, 4) != 0) { FAIL_MSG(TN, "blob mismatch"); goto fail; }

    /* erase key */
    err = nvs_erase_key(h, "val_i32");
    if (err != ESP_OK) { FAIL_MSG(TN, "erase_key: %s", esp_err_to_name(err)); goto fail; }
    err = nvs_commit(h);
    if (err != ESP_OK) { FAIL_MSG(TN, "commit erase: %s", esp_err_to_name(err)); goto fail; }

    err = nvs_get_i32(h, "val_i32", &v32);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        FAIL_MSG(TN, "expected NOT_FOUND after erase, got %s", esp_err_to_name(err));
        goto fail;
    }

    /* erase all */
    err = nvs_erase_all(h);
    if (err != ESP_OK) { FAIL_MSG(TN, "erase_all: %s", esp_err_to_name(err)); goto fail; }
    err = nvs_commit(h);
    if (err != ESP_OK) { FAIL_MSG(TN, "commit erase_all: %s", esp_err_to_name(err)); goto fail; }

    nvs_close(h);
    return true;

fail:
    nvs_close(h);
    return false;
}

/* --------------- 2. Partition table tests --------------- */

bool test_partition_table(void)
{
    const char *TN = "partition_table";

    /* Find factory app */
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        FAIL_MSG(TN, "factory app partition not found");
        return false;
    }
    ESP_LOGI(TAG, "  factory: label='%s' addr=0x%"PRIx32" size=0x%"PRIx32,
             factory->label, (uint32_t)factory->address, (uint32_t)factory->size);
    if (factory->address == 0 || factory->size == 0) {
        FAIL_MSG(TN, "factory addr/size is zero");
        return false;
    }

    /* Find NVS partition */
    const esp_partition_t *nvs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (!nvs_part) {
        FAIL_MSG(TN, "NVS partition not found");
        return false;
    }
    ESP_LOGI(TAG, "  nvs: label='%s' addr=0x%"PRIx32" size=0x%"PRIx32,
             nvs_part->label, (uint32_t)nvs_part->address, (uint32_t)nvs_part->size);

    /* Count total partitions via iterator */
    int count = 0;
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        count++;
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    ESP_LOGI(TAG, "  total partitions: %d", count);
    if (count < 2) {
        FAIL_MSG(TN, "expected at least 2 partitions, got %d", count);
        return false;
    }

    return true;
}

/* --------------- 3. SPI flash via partition API --------------- */

bool test_spi_flash(void)
{
    const char *TN = "spi_flash";
    esp_err_t err;

    /* Read first bytes of factory partition - should be ESP image magic 0xE9 */
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        FAIL_MSG(TN, "factory partition not found");
        return false;
    }

    uint8_t header[16];
    err = esp_partition_read(factory, 0, header, sizeof(header));
    if (err != ESP_OK) {
        FAIL_MSG(TN, "partition_read: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "  factory header[0]=0x%02x", header[0]);
    if (header[0] != 0xE9) {
        FAIL_MSG(TN, "expected magic 0xE9, got 0x%02x", header[0]);
        return false;
    }

    /* Get flash chip size */
    uint32_t flash_size = 0;
    err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_flash_get_size: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "  flash size: %"PRIu32" bytes (%"PRIu32" MB)",
             flash_size, flash_size / (1024 * 1024));
    if (flash_size == 0) {
        FAIL_MSG(TN, "flash size is 0");
        return false;
    }

    return true;
}

/* --------------- 4. spi_flash_mmap --------------- */

bool test_mmap(void)
{
    const char *TN = "mmap";
    esp_err_t err;

    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        FAIL_MSG(TN, "factory partition not found");
        return false;
    }

    esp_partition_mmap_handle_t mmap_handle;
    const void *mapped_ptr = NULL;
    err = esp_partition_mmap(factory, 0, factory->size,
                             ESP_PARTITION_MMAP_DATA, &mapped_ptr, &mmap_handle);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "esp_partition_mmap: %s", esp_err_to_name(err));
        return false;
    }

    /* First byte of the factory partition should be ESP image magic */
    uint8_t first_byte = *(const uint8_t *)mapped_ptr;
    ESP_LOGI(TAG, "  mmap ptr=%p first_byte=0x%02x", mapped_ptr, first_byte);
    if (first_byte != 0xE9) {
        FAIL_MSG(TN, "expected 0xE9 via mmap, got 0x%02x", first_byte);
        esp_partition_munmap(mmap_handle);
        return false;
    }

    esp_partition_munmap(mmap_handle);
    return true;
}
