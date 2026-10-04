idf_component_register(SRCS "flash32_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_partition
                       PRIV_REQUIRES spi_flash)
