/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* NEC frames from an IR receiver module on GPIO 4, timed by RMT RX through
 * the stock esp_driver_rmt and decoded as ESP-IDF's
 * examples/peripherals/rmt/ir_nec_transceiver decodes them.
 *
 * The receiver runs at 1 MHz with the example's limits: a pulse under
 * 1250 ns is noise, and 12 ms of a steady line ends a frame. Each frame is
 * decoded and the receiver armed again before anything is printed, so a
 * frame that follows closely is not missed.
 *
 * What it prints, one line each:
 *   IR: ready pin=4                       once the receiver is armed
 *   IR: address=FF00 command=E31C         each NEC frame, the two 16-bit words
 *   IR: unknown frame symbols=N           anything else
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/rmt_rx.h"

#define IR_GPIO        4
#define RESOLUTION_HZ  1000000 /* 1 us a tick */
#define MARGIN_US      200     /* the example's decode tolerance */
#define MAX_SYMBOLS    64

static bool near(uint32_t got, uint32_t want)
{
    return got < want + MARGIN_US && got + MARGIN_US > want;
}

/* A normal frame is 34 symbols: the leader (9 ms burst, 4.5 ms space), 32
 * bits least significant first (a 560 us burst, then 560 us for a 0 or
 * 1690 us for a 1), and the closing burst. */
static bool nec_decode(const rmt_symbol_word_t *s, size_t n, uint16_t *address, uint16_t *command)
{
    if (n != 34 || !near(s[0].duration0, 9000) || !near(s[0].duration1, 4500)) {
        return false;
    }
    uint32_t word = 0;
    for (int i = 0; i < 32; i++) {
        const rmt_symbol_word_t *bit = &s[1 + i];
        if (!near(bit->duration0, 560)) {
            return false;
        }
        if (near(bit->duration1, 1690)) {
            word |= 1u << i;
        } else if (!near(bit->duration1, 560)) {
            return false;
        }
    }
    *address = word & 0xFFFF;
    *command = word >> 16;
    return true;
}

static bool on_recv_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)ctx, ev, &woken);
    return woken == pdTRUE;
}

void app_main(void)
{
    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RESOLUTION_HZ,
        .mem_block_symbols = MAX_SYMBOLS,
        .gpio_num = IR_GPIO,
    };
    rmt_channel_handle_t rx = NULL;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &rx));

    QueueHandle_t done = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_recv_done };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx, &cbs, done));
    ESP_ERROR_CHECK(rmt_enable(rx));

    rmt_receive_config_t rcv = {
        .signal_range_min_ns = 1250,
        .signal_range_max_ns = 12000000,
    };
    static rmt_symbol_word_t symbols[MAX_SYMBOLS];
    ESP_ERROR_CHECK(rmt_receive(rx, symbols, sizeof(symbols), &rcv));
    printf("IR: ready pin=%d\n", IR_GPIO);
    fflush(stdout);

    for (;;) {
        rmt_rx_done_event_data_t ev;
        xQueueReceive(done, &ev, portMAX_DELAY);
        uint16_t address = 0, command = 0;
        size_t n = ev.num_symbols;
        bool nec = nec_decode(ev.received_symbols, n, &address, &command);
        ESP_ERROR_CHECK(rmt_receive(rx, symbols, sizeof(symbols), &rcv));
        if (nec) {
            printf("IR: address=%04X command=%04X\n", address, command);
        } else {
            printf("IR: unknown frame symbols=%u\n", (unsigned)n);
        }
        fflush(stdout);
    }
}
