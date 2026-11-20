/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A DHT22 temperature and humidity sensor on GPIO 5, read by timing its
 * reply with the stock GPIO driver and esp_timer. ESP-IDF has no driver for
 * the sensor, so the firmware measures every level itself.
 *
 * The host pulls the line low for 2 ms and lets go. The sensor answers with
 * an 80 us low and an 80 us high, then 40 bits, each a 50 us low and a high
 * of 26 us for a 0 or 70 us for a 1: the humidity and the temperature in
 * tenths, then a checksum. A read every half second: the first two with the
 * pin open-drain, the line let go by writing 1, the rest push-pull, the line
 * let go by switching the pin to input.
 *
 * What it prints, one line each:
 *   DHT22: ready pin=5
 *   DHT22: read round=N mode=od|pp rh=45.0 t=23.5 lo=80 hi=80
 *   DHT22: fail round=N mode=M stage=S lo=L hi=H     no reply, or one cut short
 *   DHT22: crc round=N mode=M got=XX want=YY
 * lo and hi are the presence pulse's halves as measured, in microseconds.
 */
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#define DATA_GPIO     5
#define START_LOW_US  2000
/* The sensor answers 20-40 us after the release, and a part run by esp-emu
 * itself does too. A part in a browser answers on a later turn of the page's
 * loop, milliseconds of emulated time later, so the wait for the reply is
 * long. */
#define REPLY_WAIT_US 100000
/* Every level of the reply is 80 us or less. */
#define LEVEL_WAIT_US 400
/* The datasheet asks for 2 s between reads; the part does not hold to it. */
#define ROUND_GAP_MS  500

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Spin until DATA_GPIO reads `level`: how long that took, or -1 after
 * `timeout_us`. */
static int wait_level(int level, int timeout_us)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(DATA_GPIO) != level) {
        if (esp_timer_get_time() - t0 > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - t0);
}

/* One read: 0, with the five bytes in `out`, or the stage it failed at. */
static int dht_read(bool open_drain, uint8_t out[5], int *lo, int *hi)
{
    gpio_set_direction(DATA_GPIO, open_drain ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_OUTPUT);
    gpio_set_level(DATA_GPIO, 0);
    esp_rom_delay_us(START_LOW_US);
    gpio_set_level(DATA_GPIO, 1);
    if (!open_drain) {
        gpio_set_direction(DATA_GPIO, GPIO_MODE_INPUT);
    }
    if (wait_level(0, REPLY_WAIT_US) < 0) {
        return -1;
    }
    /* No interrupt between the levels: a tick handler inside a bit's high
     * would read as a longer or shorter bit. */
    int rc = 0;
    portENTER_CRITICAL(&s_lock);
    if ((*lo = wait_level(1, LEVEL_WAIT_US)) < 0) {
        rc = -2;
    } else if ((*hi = wait_level(0, LEVEL_WAIT_US)) < 0) {
        rc = -3;
    }
    for (int i = 0; rc == 0 && i < 40; i++) {
        if (wait_level(1, LEVEL_WAIT_US) < 0) {
            rc = -4;
            break;
        }
        int h = wait_level(0, LEVEL_WAIT_US);
        if (h < 0) {
            rc = -5;
            break;
        }
        /* 26 us against 70 us: the midpoint is far from either. */
        out[i / 8] = (uint8_t)((out[i / 8] << 1) | (h > 48));
    }
    portEXIT_CRITICAL(&s_lock);
    return rc;
}

void app_main(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << DATA_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    printf("DHT22: ready pin=%d\n", DATA_GPIO);

    for (int round = 0;; round++) {
        vTaskDelay(pdMS_TO_TICKS(ROUND_GAP_MS));
        bool od = round < 2;
        const char *mode = od ? "od" : "pp";
        uint8_t f[5] = {0};
        int lo = 0, hi = 0;
        int rc = dht_read(od, f, &lo, &hi);
        if (rc < 0) {
            printf("DHT22: fail round=%d mode=%s stage=%d lo=%d hi=%d\n", round, mode, rc, lo, hi);
            continue;
        }
        uint8_t sum = (uint8_t)(f[0] + f[1] + f[2] + f[3]);
        if (sum != f[4]) {
            printf("DHT22: crc round=%d mode=%s got=%02X want=%02X\n", round, mode, f[4], sum);
            continue;
        }
        int rh = (f[0] << 8) | f[1];
        int t = ((f[2] & 0x7F) << 8) | f[3];
        printf("DHT22: read round=%d mode=%s rh=%d.%d t=%s%d.%d lo=%d hi=%d\n", round, mode, rh / 10, rh % 10,
               (f[2] & 0x80) ? "-" : "", t / 10, t % 10, lo, hi);
    }
}
