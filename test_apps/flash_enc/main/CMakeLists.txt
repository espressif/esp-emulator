idf_component_register(SRCS "flash_enc_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_partition
                       PRIV_REQUIRES spi_flash bootloader_support efuse nvs_flash)
