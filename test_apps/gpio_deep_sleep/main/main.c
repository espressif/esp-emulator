/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Deep sleep until a button on GPIO 3 is pressed.
 *
 * The button shorts the pin to ground against the internal pull-up, so the
 * chip wakes on a low level. GPIO 3 is one of the pads the RTC domain keeps
 * watching in deep sleep (GPIO 0 to 5 on ESP32-C3). The first boot arms the
 * wake-up and sleeps, with no timer; the boot after the wake says what woke
 * it and stops.
 */
#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_idf_version.h"
#include "esp_sleep.h"
#include "esp_system.h"

#define BUTTON_GPIO 3

void app_main(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        printf("SLEEP: woke reset_reason=%d cause=%d pins=0x%" PRIx64 "\n", (int)esp_reset_reason(), (int)cause,
               esp_sleep_get_gpio_wakeup_status());
        fflush(stdout);
        return;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << BUTTON_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW));
#else
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(1ULL << BUTTON_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW));
#endif

    printf("SLEEP: sleeping level=%d\n", gpio_get_level(BUTTON_GPIO));
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_deep_sleep_start();
}
