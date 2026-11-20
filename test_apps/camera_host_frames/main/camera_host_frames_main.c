/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The stock esp_driver_cam DVP controller on ESP32-P4, capturing JPEG frames
 * the emulator's host source feeds it:
 *
 *   esp-emu --chip esp32p4 --firmware build/merged_flash.bin \
 *           --camera-frames DIR_OF_JPEG_FRAMES --camera-fps 30 \
 *           --exit-on "RESULT_SUMMARY:"
 *
 * Each frame is checked for its SOI at the start and its EOI at exactly the
 * reported length, and printed with its size and CRC32 for the host to hold
 * against the file it sent:
 *
 *   FRAME <n> size=<bytes> crc32=<hex>
 *   RESULT_SUMMARY: passed=N failed=M target=esp32p4
 *
 * The frame buffers are internal DMA memory: JPEG is accounted at a byte a
 * pixel, so a 320x240 buffer holds the ~1 KB fixtures many times over. With
 * bk_buffer_dis set the driver asserts when on_get_new_trans finds no buffer,
 * so two buffers circulate between the callbacks and the main task. */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_attr.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

static const char *TAG = "camera_host_frames";

/* Ordinary GPIOs through the matrix, as the ESP32-P4-Function-EV-Board's
 * DVP examples use: P4's dedicated CAM pads include GPIOs past 54. */
#define PIN_XCLK  20
#define PIN_PCLK  21
#define PIN_DE    22
#define PIN_VSYNC 23

#define H_RES 320
#define V_RES 240
#define FB_SIZE (H_RES * V_RES)
#define FB_COUNT 2
#define FRAMES_WANTED 8

static uint8_t *s_fb[FB_COUNT];
static QueueHandle_t s_free_q;
static QueueHandle_t s_done_q;
static int s_passed;
static int s_failed;

static void check(bool ok, const char *what, int frame)
{
    if (ok) {
        s_passed++;
    } else {
        s_failed++;
        printf("FAIL frame %d: %s\n", frame, what);
    }
}

static bool IRAM_ATTR on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    BaseType_t woken = pdFALSE;
    uint8_t *buf = NULL;
    if (xQueueReceiveFromISR(s_free_q, &buf, &woken) == pdTRUE) {
        trans->buffer = buf;
        trans->buflen = FB_SIZE;
    }
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_done_q, trans, &woken);
    return woken == pdTRUE;
}

static void finish(void)
{
    printf("RESULT_SUMMARY: passed=%d failed=%d target=esp32p4\n", s_passed, s_failed);
}

void app_main(void)
{
    s_free_q = xQueueCreate(FB_COUNT, sizeof(uint8_t *));
    s_done_q = xQueueCreate(FB_COUNT, sizeof(esp_cam_ctlr_trans_t));
    for (int i = 0; i < FB_COUNT; i++) {
        s_fb[i] = heap_caps_aligned_alloc(64, FB_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_fb[i]) {
            check(false, "frame buffer allocation", -1);
            finish();
            return;
        }
        /* A frame never written cannot pass the checks on old heap contents. */
        memset(s_fb[i], 0xA5, FB_SIZE);
        xQueueSend(s_free_q, &s_fb[i], 0);
    }

    esp_cam_ctlr_dvp_pin_config_t pin_cfg = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {53, 54, 26, 1, 0, 45, 46, 47},
        .vsync_io = PIN_VSYNC,
        .de_io = PIN_DE,
        .pclk_io = PIN_PCLK,
        .xclk_io = PIN_XCLK,
    };
    esp_cam_ctlr_dvp_config_t cfg = {
        .ctlr_id = 0,
        .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = H_RES,
        .v_res = V_RES,
        .dma_burst_size = 64,
        .pin = &pin_cfg,
        .xclk_freq = 20000000,
        .bk_buffer_dis = true,
        .pic_format_jpeg = true,
    };
    esp_cam_ctlr_handle_t cam = NULL;
    esp_err_t err = esp_cam_new_dvp_ctlr(&cfg, &cam);
    if (err != ESP_OK) {
        printf("FAIL: esp_cam_new_dvp_ctlr -> %s\n", esp_err_to_name(err));
        s_failed++;
        finish();
        return;
    }
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(cam, &cbs, NULL));
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_start(cam));
    ESP_LOGI(TAG, "capturing %d frames at %dx%d", FRAMES_WANTED, H_RES, V_RES);

    for (int n = 0; n < FRAMES_WANTED; n++) {
        esp_cam_ctlr_trans_t trans;
        /* A run with no host source fails here rather than hangs. */
        if (xQueueReceive(s_done_q, &trans, pdMS_TO_TICKS(2000)) != pdTRUE) {
            check(false, "no frame within 2000 ms", n);
            continue;
        }
        const uint8_t *b = trans.buffer;
        size_t len = trans.received_size;
        bool plausible = len > 100 && len <= FB_SIZE;
        check(plausible, "implausible received_size", n);
        if (plausible) {
            check(b[0] == 0xFF && b[1] == 0xD8, "no SOI at offset 0", n);
            /* A frame reported longer than it is still holds an EOI somewhere. */
            check(b[len - 2] == 0xFF && b[len - 1] == 0xD9, "no EOI at the end", n);
            printf("FRAME %d size=%u crc32=%08x\n", n, (unsigned)len, (unsigned)esp_rom_crc32_le(0, b, len));
        }
        memset(trans.buffer, 0xA5, FB_SIZE);
        uint8_t *back = trans.buffer;
        xQueueSend(s_free_q, &back, 0);
    }

    esp_cam_ctlr_stop(cam);
    esp_cam_ctlr_disable(cam);
    esp_cam_ctlr_del(cam);
    finish();
}
