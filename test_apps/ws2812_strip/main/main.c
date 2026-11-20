/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A WS2812 strip of eight pixels on GPIO 8, driven by the stock RMT transmit
 * driver with a copy encoder: the firmware builds every symbol itself, so
 * no managed component is fetched.
 *
 * At 10 MHz a 0 is 400 ns high and 800 ns low, a 1 800 ns high and 400 ns
 * low, 24 bits a pixel, green, red, blue, most significant bit first; a
 * 120 us low ends the frame. Frames go out a fifth of a second apart: two
 * fixed patterns, A then B then A, then a turning rainbow.
 *
 * What it prints, one line each:
 *   STRIP: ready pin=8 pixels=8
 *   STRIP: frame N sent              once frame N has left the pin; the next
 *                                    follows a fifth of a second later
 */
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"

#define DATA_GPIO     8
#define PIXELS        8
/* 10 MHz: one tick is 100 ns, and the datasheet's times are whole ticks. */
#define RESOLUTION_HZ 10000000
#define T0H 4
#define T0L 8
#define T1H 8
#define T1L 4
/* The latch: 120 us of low, in two halves of one symbol. */
#define RESET_TICKS   600

static rmt_symbol_word_t s_symbols[PIXELS * 24 + 1];

/* One frame of `rgb` as symbols, GRB on the wire. */
static size_t encode(const uint8_t rgb[PIXELS][3])
{
    size_t k = 0;
    for (int i = 0; i < PIXELS; i++) {
        const uint8_t grb[3] = { rgb[i][1], rgb[i][0], rgb[i][2] };
        for (int b = 0; b < 3; b++) {
            for (int bit = 7; bit >= 0; bit--) {
                bool one = (grb[b] >> bit) & 1;
                s_symbols[k++] = (rmt_symbol_word_t) {
                    .level0 = 1, .duration0 = one ? T1H : T0H,
                    .level1 = 0, .duration1 = one ? T1L : T0L,
                };
            }
        }
    }
    s_symbols[k++] = (rmt_symbol_word_t) { .level0 = 0, .duration0 = RESET_TICKS, .level1 = 0, .duration1 = RESET_TICKS };
    return k;
}

/* Hue `h` of 0-255 at a quarter brightness. */
static void wheel(uint8_t h, uint8_t out[3])
{
    uint8_t r, g, b;
    if (h < 85) {
        r = 255 - h * 3; g = h * 3; b = 0;
    } else if (h < 170) {
        h -= 85; r = 0; g = 255 - h * 3; b = h * 3;
    } else {
        h -= 170; r = h * 3; g = 0; b = 255 - h * 3;
    }
    out[0] = r / 4; out[1] = g / 4; out[2] = b / 4;
}

void app_main(void)
{
    rmt_channel_handle_t chan = NULL;
    rmt_tx_channel_config_t cfg = {
        .gpio_num = DATA_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg, &chan));
    rmt_encoder_handle_t enc = NULL;
    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, &enc));
    ESP_ERROR_CHECK(rmt_enable(chan));
    printf("STRIP: ready pin=%d pixels=%d\n", DATA_GPIO, PIXELS);

    /* Colours a decode error cannot pass: red and green apart, every bit
     * of a byte used somewhere, and the strip's ends distinct. */
    const uint8_t a[PIXELS][3] = {
        { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 1, 2, 3 },
        { 255, 255, 255 }, { 0, 0, 0 }, { 16, 32, 48 }, { 128, 64, 7 },
    };
    const uint8_t b[PIXELS][3] = {
        { 7, 0, 7 }, { 0, 0, 0 }, { 255, 255, 255 }, { 0, 128, 0 },
        { 48, 32, 16 }, { 3, 2, 1 }, { 0, 0, 255 }, { 255, 0, 0 },
    };
    uint8_t rainbow[PIXELS][3];
    rmt_transmit_config_t tx = { .loop_count = 0 };
    for (int n = 0;; n++) {
        const uint8_t (*f)[3] = n == 1 ? b : a;
        if (n > 2) {
            for (int i = 0; i < PIXELS; i++) {
                wheel((uint8_t)(n * 8 + i * 256 / PIXELS), rainbow[i]);
            }
            f = rainbow;
        }
        size_t k = encode(f);
        ESP_ERROR_CHECK(rmt_transmit(chan, enc, s_symbols, k * sizeof s_symbols[0], &tx));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(chan, 1000));
        printf("STRIP: frame %d sent\n", n);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
