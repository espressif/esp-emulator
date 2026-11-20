/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A microphone played back out to an amplifier, through the stock i2s_std
 * driver.
 *
 * The firmware side of the co-simulation I2S channel: an INMP441 microphone
 * drives the data input and a MAX98357A amplifier listens on the data output,
 * and nothing here knows they are parts. One port both ways, as a codec board
 * drives it: the TX and RX handles of one i2s_new_channel share BCLK and WS.
 * 16 kHz, Philips format, 32-bit slots in stereo: the INMP441 sends 24 bits
 * in a 32-bit slot, 64 BCLK a frame (InvenSense INMP441 datasheet), and the
 * MAX98357A takes 16, 24 or 32 (Maxim MAX98357A datasheet).
 *
 * BCLK on GPIO 4, WS on 5, data out on 6 and data in on 7, on every target.
 *
 * Each 100 ms block read is written back out unchanged, and measured on the
 * left slot, where a microphone with L/R low puts its samples. Once a second
 * it prints the pitch, from the upward zero crossings in that second; the
 * peak, in dBFS; and how much of the second was silence, counting every run
 * of zeros of 1 ms or more, which a tone never has:
 *
 *   I2S_ECHO: tone=440 Hz peak=-12 dBFS silent=0 ms
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_err.h"

#define RATE_HZ 16000
#define BLOCK_FRAMES (RATE_HZ / 10)
#define BLOCKS_PER_REPORT 10
/* A run of zeros this long, 1 ms, is a gap in the sound. */
#define GAP_FRAMES (RATE_HZ / 1000)

static int32_t block[BLOCK_FRAMES * 2];

void app_main(void)
{
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Zeros, not the last buffer again, when the playback runs dry. */
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, &rx));

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_4,
            .ws = GPIO_NUM_5,
            .dout = GPIO_NUM_6,
            .din = GPIO_NUM_7,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    printf("I2S_ECHO: started rate=%d bits=32 slots=2\n", RATE_HZ);

    uint32_t crossings = 0, zeros = 0, silent = 0, blocks = 0;
    int64_t peak = 0;
    int32_t prev = 0;
    for (;;) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(rx, block, sizeof block, &got, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            printf("I2S_ECHO: read err=%s\n", esp_err_to_name(err));
            fflush(stdout);
            continue;
        }
        size_t sent = 0;
        i2s_channel_write(tx, block, got, &sent, pdMS_TO_TICKS(1000));

        for (size_t i = 0; i < got / sizeof(int32_t[2]); i++) {
            int32_t left = block[2 * i];
            int64_t mag = left < 0 ? -(int64_t)left : left;
            if (mag > peak) {
                peak = mag;
            }
            if (prev < 0 && left >= 0) {
                crossings++;
            }
            prev = left;
            if (left == 0) {
                zeros++;
            } else {
                if (zeros >= GAP_FRAMES) {
                    silent += zeros;
                }
                zeros = 0;
            }
        }
        if (++blocks < BLOCKS_PER_REPORT) {
            continue;
        }
        if (zeros >= GAP_FRAMES) {
            silent += zeros;
        }
        int db = peak ? (int)lroundf(20.0f * log10f((float)peak / 2147483648.0f)) : -99;
        printf("I2S_ECHO: tone=%lu Hz peak=%d dBFS silent=%lu ms\n", (unsigned long)crossings, db,
               (unsigned long)(silent * 1000 / RATE_HZ));
        fflush(stdout);
        crossings = silent = zeros = blocks = 0;
        peak = 0;
    }
}
