/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * NVS, partition table, SPI flash, flash mmap and the SD card.
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

/* --------------- SD card --------------- */

/* The chip select of a card on SPI2, on the chips with no SDMMC host: a pad
 * no other case uses, and the one the emulator is told (`--sd FILE,cs=3`). */
#define SD_SPI_CS_GPIO 3
#define SD_TEST_SECTOR 64
/* On the SDMMC host, more than the driver's DMA descriptor ring holds
 * (16 KiB), so the transfer goes on after a refill. */
#if SOC_SDMMC_HOST_SUPPORTED
#define SD_TEST_SECTORS 64
#else
#define SD_TEST_SECTORS 4
#endif

/* Write a pattern to a few blocks of the card and read it back. */
static bool sd_card_round_trip(const char *TN, sdmmc_card_t *card)
{
    const size_t len = SD_TEST_SECTORS * 512;
    uint8_t *wr = heap_caps_malloc(len, MALLOC_CAP_DMA);
    uint8_t *rd = heap_caps_calloc(1, len, MALLOC_CAP_DMA);
    bool ok = false;
    if (!wr || !rd) {
        FAIL_MSG(TN, "no memory for %u bytes", (unsigned)len);
        goto out;
    }
    for (size_t i = 0; i < len; i++) {
        wr[i] = (uint8_t)(i * 7 + (i >> 9));
    }
    esp_err_t err = sdmmc_write_sectors(card, wr, SD_TEST_SECTOR, SD_TEST_SECTORS);
    if (err != ESP_OK) { FAIL_MSG(TN, "write: %s", esp_err_to_name(err)); goto out; }
    err = sdmmc_read_sectors(card, rd, SD_TEST_SECTOR, SD_TEST_SECTORS);
    if (err != ESP_OK) { FAIL_MSG(TN, "read: %s", esp_err_to_name(err)); goto out; }
    if (memcmp(wr, rd, len) != 0) { FAIL_MSG(TN, "read back differs"); goto out; }
    /* The line the emulator's own suite looks for: a card was really there. */
    ESP_LOGI(TAG, "  sd_card: %u blocks of %u read back", SD_TEST_SECTORS, (unsigned)card->csd.capacity);
    ok = true;
out:
    free(wr);
    free(rd);
    return ok;
}

/* A card in the slot is optional: a board, or an emulator run without --sd,
 * has none, and that is not this case's failure. */
static bool sd_no_card(const char *TN, esp_err_t err)
{
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE) {
        ESP_LOGI(TAG, "  sd_card: no card (%s), skipped", esp_err_to_name(err));
        return true;
    }
    FAIL_MSG(TN, "card init: %s", esp_err_to_name(err));
    return false;
}

bool test_sd_card(void)
{
    const char *TN = "sd_card";
    sdmmc_card_t card;
    bool ok;
#if SOC_SDMMC_HOST_SUPPORTED
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    host.flags &= ~(SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_8BIT);
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) { FAIL_MSG(TN, "host init: %s", esp_err_to_name(err)); return false; }
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "slot init: %s", esp_err_to_name(err));
        sdmmc_host_deinit();
        return false;
    }
    err = sdmmc_card_init(&host, &card);
    ok = (err == ESP_OK) ? sd_card_round_trip(TN, &card) : sd_no_card(TN, err);
    sdmmc_host_deinit();
#else
    spi_bus_config_t bus = {
        .sclk_io_num = SPI2_IOMUX_PIN_NUM_CLK,
        .mosi_io_num = SPI2_IOMUX_PIN_NUM_MOSI,
        .miso_io_num = SPI2_IOMUX_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) { FAIL_MSG(TN, "spi bus: %s", esp_err_to_name(err)); return false; }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = SPI2_HOST;
    dev.gpio_cs = SD_SPI_CS_GPIO;
    sdspi_dev_handle_t handle;
    err = sdspi_host_init();
    if (err == ESP_OK) {
        err = sdspi_host_init_device(&dev, &handle);
    }
    if (err != ESP_OK) {
        FAIL_MSG(TN, "sdspi init: %s", esp_err_to_name(err));
        sdspi_host_deinit();
        spi_bus_free(SPI2_HOST);
        return false;
    }
    host.slot = handle;
    err = sdmmc_card_init(&host, &card);
    ok = (err == ESP_OK) ? sd_card_round_trip(TN, &card) : sd_no_card(TN, err);
    sdspi_host_remove_device(handle);
    sdspi_host_deinit();
    spi_bus_free(SPI2_HOST);
#endif
    return ok;
}
