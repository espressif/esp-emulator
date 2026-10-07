# `hal/ecdsa_hal.h` moved from the `hal` component to `esp_hal_security` in
# IDF v6.0. Require whichever this IDF ships.
if(EXISTS "$ENV{IDF_PATH}/components/esp_hal_security")
    set(ecdsa_hal_req esp_hal_security)
else()
    set(ecdsa_hal_req hal)
endif()

idf_component_register(SRCS "test_main.c"
                            "test_storage.c"
                            "test_periph.c"
                            "test_timer.c"
                            "test_freertos.c"
                            "test_system.c"
                            "test_net.c"
                            "test_ble.c"
                            "test_crypto.c"
                            "test_reset.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_driver_gpio esp_driver_rmt esp_driver_pcnt esp_driver_ledc esp_driver_i2c esp_driver_spi esp_timer esp_event esp_netif pthread esp_eth bt
                       PRIV_REQUIRES spi_flash esp_partition app_update esp_wifi mbedtls esp_hw_support esp_driver_gptimer ${ecdsa_hal_req} esp_security efuse)
