/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* One ADC pad converted through the stock adc_continuous driver, at 20 kHz
 * into 256-byte frames: adc_oneshot's pad and range (GPIO 3, GPIO 43 on the
 * S31, GPIO 20 on the P4; 12 dB, 0 dB on the S31).
 *
 * Once a second: `ADC: read=N raw=<first code> mv=<mean, calibrated>
 * rate=<conversions a second> samples=<codes of the channel in the frame>`.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#if CONFIG_IDF_TARGET_ESP32S31
#define PAD 43
#define ATTEN ADC_ATTEN_DB_0
#elif CONFIG_IDF_TARGET_ESP32P4
#define PAD 20
#define ATTEN ADC_ATTEN_DB_12
#else
#define PAD 3
#define ATTEN ADC_ATTEN_DB_12
#endif

#define SAMPLE_HZ 20000
#define FRAME_BYTES 256
#define FRAME_SAMPLES (FRAME_BYTES / SOC_ADC_DIGI_RESULT_BYTES)

static adc_cali_handle_t s_cali;
static volatile uint32_t s_frames;

static bool IRAM_ATTR on_frame(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user)
{
    s_frames = s_frames + 1;
    return false;
}

static bool cali_init(adc_unit_t unit, adc_channel_t chan)
{
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = unit, .chan = chan, .atten = ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cfg, &s_cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    (void)chan;
    adc_cali_line_fitting_config_t cfg = {
        .unit_id = unit, .atten = ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_line_fitting(&cfg, &s_cali);
#endif
    printf("ADC_CONT: calibration %s\n", err == ESP_OK ? "on" : esp_err_to_name(err));
    return err == ESP_OK;
}

/* The S31's scheme converts by channel; the others' do not take one. */
static esp_err_t to_millivolts(adc_channel_t chan, int raw, int *mv)
{
#if CONFIG_IDF_TARGET_ESP32S31
    return adc_cali_chan_raw_to_voltage(s_cali, chan, raw, mv);
#else
    (void)chan;
    return adc_cali_raw_to_voltage(s_cali, raw, mv);
#endif
}

void app_main(void)
{
    adc_unit_t unit;
    adc_channel_t chan;
    ESP_ERROR_CHECK(adc_continuous_io_to_channel(PAD, &unit, &chan));

    adc_continuous_handle_t adc;
    adc_continuous_handle_cfg_t handle_cfg = { .max_store_buf_size = 4 * FRAME_BYTES, .conv_frame_size = FRAME_BYTES };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc));
    adc_digi_pattern_config_t pattern = {
        .atten = ATTEN, .channel = chan, .unit = unit, .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t cfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = SAMPLE_HZ,
        .conv_mode = unit == ADC_UNIT_1 ? ADC_CONV_SINGLE_UNIT_1 : ADC_CONV_SINGLE_UNIT_2,
#if !CONFIG_IDF_TARGET_ESP32S31
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
#endif
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc, &cfg));
    adc_continuous_evt_cbs_t cbs = { .on_conv_done = on_frame };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc, &cbs, NULL));
    printf("ADC_CONT: gpio=%d unit=%d channel=%d\n", PAD, unit + 1, chan);
    bool cali = cali_init(unit, chan);

    ESP_ERROR_CHECK(adc_continuous_start(adc));
    static uint8_t frame[FRAME_BYTES];
    static adc_continuous_data_t parsed[FRAME_SAMPLES];
    uint32_t frames_before = s_frames;
    int64_t since = esp_timer_get_time();
    for (int n = 0;; n++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t frames = s_frames;
        int64_t now = esp_timer_get_time();
        uint32_t rate = (uint64_t)(frames - frames_before) * FRAME_SAMPLES * 1000000 / (now - since);
        frames_before = frames;
        since = now;

        /* The frames the pool kept are from before the wait: empty it, then
         * take the next one. */
        uint32_t len = 0, count = 0;
        while (adc_continuous_read(adc, frame, FRAME_BYTES, &len, 0) == ESP_OK) {
        }
        ESP_ERROR_CHECK(adc_continuous_read(adc, frame, FRAME_BYTES, &len, 1000));
        ESP_ERROR_CHECK(adc_continuous_parse_data(adc, frame, len, parsed, &count));
        /* Each code is read through the calibration on its own: the S31's
         * word is not a count that codes can be averaged in. */
        int64_t mv_sum = 0;
        uint32_t valid = 0, converted = 0;
        int raw = -1;
        for (uint32_t i = 0; i < count; i++) {
            if (!parsed[i].valid || parsed[i].unit != unit || parsed[i].channel != chan) {
                continue;
            }
            if (valid++ == 0) {
                raw = parsed[i].raw_data;
            }
            int mv = 0;
            if (cali && to_millivolts(chan, parsed[i].raw_data, &mv) == ESP_OK) {
                mv_sum += mv;
                converted++;
            }
        }
        if (converted) {
            printf("ADC: read=%d raw=%d mv=%d rate=%lu samples=%lu\n", n, raw, (int)(mv_sum / converted),
                   (unsigned long)rate, (unsigned long)valid);
        } else {
            printf("ADC: read=%d raw=%d mv=- rate=%lu samples=%lu\n", n, raw, (unsigned long)rate, (unsigned long)valid);
        }
    }
}
