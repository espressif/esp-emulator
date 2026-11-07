/* A button on GPIO 9, read through a real interrupt.
 *
 * Deliberately the smallest firmware that proves the inbound half of the
 * co-simulation bus: something outside the emulator moves a pin, and the
 * guest's ISR runs. Nothing here knows the button is virtual.
 *
 * The pin is input with a pull-up and the button shorts it to ground, which is
 * what parts/pushbutton describes: "momentary contact; use internal pullup,
 * active low". An undriven pin therefore reads HIGH and a press reads LOW.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#define BUTTON_GPIO 9

static QueueHandle_t edges;

static void IRAM_ATTR on_edge(void *arg)
{
    int level = gpio_get_level(BUTTON_GPIO);
    xQueueSendFromISR(edges, &level, NULL);
}

void app_main(void)
{
    edges = xQueueCreate(8, sizeof(int));

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, on_edge, NULL));

    /* The starting level, so a watcher can tell "never pressed" from "pressed
     * before I was looking". */
    printf("BUTTON: ready level=%d\n", gpio_get_level(BUTTON_GPIO));
    fflush(stdout);

    int level;
    for (;;) {
        if (xQueueReceive(edges, &level, portMAX_DELAY) == pdTRUE) {
            printf("BUTTON: %s\n", level ? "released" : "pressed");
            fflush(stdout);
        }
    }
}
