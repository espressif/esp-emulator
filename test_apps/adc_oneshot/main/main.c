/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* One ADC pad read through the stock adc_oneshot driver.
 *
 * The firmware side of the co-simulation analog channel: a part holds a
 * voltage on the pad (a potentiometer's wiper, a thermistor divider's
 * output), and nothing here knows it is virtual.
 *
 * The pad is GPIO 3, GPIO 43 on the S31 (ADC1 channel 1, a P input) and
 * GPIO 20 on the P4 (ADC1 channel 4), read at 12 dB, at 0 dB on the S31, the
 * one range it has. The millivolts come from
 * the chip's calibration scheme (curve fitting, line fitting on the S31);
 * where the eFuse carries no calibration the line says `mv=-`.
 *
 * It reads the pad three times, prints `ADC_ONESHOT: done`, then reads it
 * once a second, so a change made to the part shows in the output.
 */
#include <stdbool.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

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

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;

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
    printf("ADC_ONESHOT: calibration %s\n", err == ESP_OK ? "on" : esp_err_to_name(err));
    return err == ESP_OK;
}

/* One reading: the raw code, and the millivolts where calibration is on. */
static void read_once(bool cali, int n)
{
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc, s_chan, &raw));
    printf("ADC: ");
    if (n >= 0) {
        printf("read=%d ", n);
    }
    int mv = 0;
    if (cali && adc_oneshot_get_calibrated_result(s_adc, s_cali, s_chan, &mv) == ESP_OK) {
        printf("raw=%d mv=%d\n", raw, mv);
    } else {
        printf("raw=%d mv=-\n", raw);
    }
}

void app_main(void)
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(PAD, &unit, &s_chan));
    adc_oneshot_unit_init_cfg_t init = { .unit_id = unit };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init, &s_adc));
    adc_oneshot_chan_cfg_t chan = { .atten = ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_chan, &chan));
    printf("ADC_ONESHOT: gpio=%d unit=%d channel=%d\n", PAD, unit + 1, s_chan);
    bool cali = cali_init(unit, s_chan);

    for (int n = 0; n < 3; n++) {
        read_once(cali, n);
    }
    printf("ADC_ONESHOT: done\n");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        read_once(cali, -1);
    }
}
