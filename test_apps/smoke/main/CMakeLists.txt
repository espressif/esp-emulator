# `hal/ecdsa_hal.h` moved from the `hal` component to `esp_hal_security` in
# IDF v6.0. Require whichever this IDF ships.
if(EXISTS "$ENV{IDF_PATH}/components/esp_hal_security")
    set(ecdsa_hal_req esp_hal_security)
else()
    set(ecdsa_hal_req hal)
endif()

set(srcs "test_main.c"
         "test_storage.c"
         "test_periph.c"
         "test_timer.c"
         "test_freertos.c"
         "test_system.c"
         "test_net.c"
         "test_ble.c"
         "test_crypto.c"
         "test_reset.c"
         "test_pie.c"
         "test_ee.c")
# PIE cases (test_pie.c): the rev-3 PIE only, which is all the emulator models.
# The case instructions are emitted as words, so any toolchain that builds the
# harness builds them. sdkconfig.pie_full swaps the committed subset for the
# full set gen.py writes to pie/full/.
if(CONFIG_SOC_CPU_HAS_PIE AND NOT CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
    if(CONFIG_EMU_TEST_PIE_FULL)
        if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/pie/full/cases.c")
            message(FATAL_ERROR "EMU_TEST_PIE_FULL: run tools/internal/pie/silicon/gen.py first")
        endif()
        set(pie_cases "pie/full")
    else()
        set(pie_cases "pie")
    endif()
    list(APPEND srcs "pie/run.c" "pie/harness.S" "${pie_cases}/cases.c" "${pie_cases}/cases.S")
    set(emu_test_pie 1)
else()
    set(emu_test_pie 0)
endif()
# ESP32-S3 TIE cases (test_ee.c), the `ee.*` counterpart: sdkconfig.ee_full
# swaps the committed subset for the full set gen_s3.py writes to ee/full/.
if(CONFIG_IDF_TARGET_ESP32S3)
    if(CONFIG_EMU_TEST_EE_FULL)
        if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/ee/full/cases.c")
            message(FATAL_ERROR "EMU_TEST_EE_FULL: run tools/internal/pie/silicon/gen_s3.py first")
        endif()
        set(ee_cases "ee/full")
    else()
        set(ee_cases "ee")
    endif()
    list(APPEND srcs "ee/run.c" "ee/harness.S" "${ee_cases}/cases.c" "${ee_cases}/cases.S")
    set(emu_test_ee 1)
    set(ee_req xtensa)
else()
    set(emu_test_ee 0)
    set(ee_req "")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_driver_gpio esp_driver_rmt esp_driver_pcnt esp_driver_ledc esp_driver_i2c esp_driver_spi esp_timer esp_event esp_netif pthread esp_eth bt
                       PRIV_REQUIRES spi_flash esp_partition app_update esp_wifi mbedtls esp_hw_support esp_driver_gptimer ${ecdsa_hal_req} esp_security efuse ${ee_req})
target_compile_definitions(${COMPONENT_LIB} PRIVATE EMU_TEST_PIE=${emu_test_pie} EMU_TEST_EE=${emu_test_ee})
