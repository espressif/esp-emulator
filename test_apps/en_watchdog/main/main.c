/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A control loop that kicks an external watchdog, and then hangs.
 *
 * The board has a windowed watchdog supervisor (examples/parts/watchdog): its
 * WDI input on GPIO 4, its reset output on the chip's EN pin. The loop toggles
 * GPIO 4 every 200 ms, inside the supervisor's window, ten times; then it
 * stops, as a hung loop does. The supervisor pulls EN, the chip is held in
 * reset for the supervisor's reset time, and this runs again from the top,
 * reporting the reset reason an EN reset has: power-on.
 *
 * Nothing here knows the supervisor is virtual, and nothing survives the
 * reset to say it happened: that is what an EN reset is.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_system.h"

#define WDI_GPIO 4
#define KICKS 10
#define KICK_PERIOD_MS 200

void app_main(void)
{
    printf("WDT: boot reset_reason=%d%s\n", esp_reset_reason(),
           esp_reset_reason() == ESP_RST_POWERON ? " (POWERON)" : "");

    /* The level first, then the output enable: the line rests high on the
     * supervisor's pull-up, and enabling the output on a latch still at 0
     * would be two edges microseconds apart, a kick inside the closed window. */
    int level = 1;
    gpio_set_level(WDI_GPIO, level);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << WDI_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    for (int n = 0; n < KICKS; n++) {
        vTaskDelay(pdMS_TO_TICKS(KICK_PERIOD_MS));
        level = !level;
        gpio_set_level(WDI_GPIO, level);
    }
    printf("WDT: kicked %d times, now hung\n", KICKS);
    fflush(stdout);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
