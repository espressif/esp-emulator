/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The stock esp_driver_cam controllers against camera sensors that are
 * co-simulated parts: an OV2640 on the DVP bus, and on ESP32-P4 an OV5647 on
 * the MIPI CSI link as well. The firmware probes each sensor over SCCB and
 * programs its output as esp_cam_sensor's drivers do (320 x 240: RGB565 low
 * byte first on DVP, RAW8 on CSI), then captures FRAMES frames and checks the
 * sensor's colour bars at the centre of each bar. The OV2640 is then put in
 * JPEG mode, which sends only a picture it was shown: each JPEG frame's size
 * and CRC32 are printed for whoever showed it one to compare, and with none
 * shown in 10 s the JPEG phase is skipped. */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "soc/soc_caps.h"
#if SOC_MIPI_CSI_SUPPORTED
#include "esp_cam_ctlr_csi.h"
#include "esp_ldo_regulator.h"
#endif

#define H_RES   320
#define V_RES   240
#define FRAMES  3

/* SCCB: I2C on two pads clear of the DVP bus. */
#define SCCB_SCL 16
#define SCCB_SDA 17
#define OV2640_ADDR 0x30
#define OV5647_ADDR 0x36

/* The same DVP pads on every target that has the block. */
static const esp_cam_ctlr_dvp_pin_config_t s_pins = {
    .data_width = CAM_CTLR_DATA_WIDTH_8,
    .data_io = {4, 5, 6, 7, 8, 9, 10, 11},
    .pclk_io = 12,
    .vsync_io = 13,
    .de_io = 14,
    .xclk_io = 15,
};

/* A sensor's colour-bar test pattern, left to right. */
static const uint8_t BARS[8][3] = {
    {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
    {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
};

typedef struct {
    size_t size;
    int wrong;   /* bars whose centre pixel is not the bar's colour */
    uint32_t crc;
} frame_t;

static i2c_master_bus_handle_t s_sccb;
static QueueHandle_t s_done;
static void *s_buf;
static size_t s_buf_len;
/* How the frame in flight is checked: by the bus it came on; a JPEG frame
 * has no bars and is summed instead. */
static int (*s_wrong_bars)(const uint8_t *buf);
static i2c_master_dev_handle_t s_ov2640;

static uint16_t rgb565(const uint8_t c[3])
{
    return (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3);
}

/* Bars wrong in a 320 x 240 RGB565 frame, low byte first, on its middle row. */
static int wrong_bars_rgb565(const uint8_t *buf)
{
    int wrong = 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t *p = buf + 2 * ((V_RES / 2) * H_RES + (2 * i + 1) * H_RES / 16);
        if ((uint16_t)(p[0] | p[1] << 8) != rgb565(BARS[i])) {
            wrong++;
        }
    }
    return wrong;
}

static bool on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user)
{
    trans->buffer = s_buf;
    trans->buflen = s_buf_len;
    return false;
}

/* The driver hands the one buffer straight back to the DMA, so the frame is
 * checked here, before the next one lands in it. */
static bool on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user)
{
    BaseType_t woken = pdFALSE;
    frame_t f = {
        .size = trans->received_size,
        .wrong = s_wrong_bars ? s_wrong_bars(trans->buffer) : 0,
        .crc = s_wrong_bars ? 0 : esp_rom_crc32_le(0, trans->buffer, trans->received_size),
    };
    xQueueSendFromISR(s_done, &f, &woken);
    return woken == pdTRUE;
}

static bool sccb_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t w[2] = {reg, val};
    return i2c_master_transmit(dev, w, sizeof(w), 100) == ESP_OK;
}

static bool sccb_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100) == ESP_OK;
}

/* Probe the OV2640 and program 320 x 240 RGB565, low byte first: the DSP
 * bank's IMAGE_MODE and output size, as esp_cam_sensor's setting writes. */
static bool ov2640_setup(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OV2640_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_sccb, &cfg, &s_ov2640));
    i2c_master_dev_handle_t dev = s_ov2640;
    uint8_t pid = 0, ver = 0;
    bool ok = sccb_write(dev, 0xFF, 0x01) && sccb_read(dev, 0x0A, &pid) && sccb_read(dev, 0x0B, &ver);
    printf("OV2640: pid=%02x ver=%02x\n", pid, ver);
    if (!ok || pid != 0x26) {
        return false;
    }
    static const uint8_t setting[][2] = {
        {0xFF, 0x00}, {0xE0, 0x04}, {0xDA, 0x09},
        {0x5A, H_RES / 4}, {0x5B, V_RES / 4}, {0x5C, 0x00}, {0xE0, 0x00},
    };
    for (size_t i = 0; i < sizeof(setting) / sizeof(setting[0]); i++) {
        ok = ok && sccb_write(dev, setting[i][0], setting[i][1]);
    }
    return ok;
}

/* JPEG at the same size: IMAGE_MODE's JPEG bit, as esp_cam_sensor's JPEG
 * settings write it. */
static bool ov2640_jpeg(void)
{
    static const uint8_t setting[][2] = {{0xFF, 0x00}, {0xE0, 0x04}, {0xDA, 0x10}, {0xE0, 0x00}};
    bool ok = true;
    for (size_t i = 0; i < sizeof(setting) / sizeof(setting[0]); i++) {
        ok = ok && sccb_write(s_ov2640, setting[i][0], setting[i][1]);
    }
    printf("OV2640: jpeg %s\n", ok ? "ok" : "failed");
    return ok;
}

static bool dvp_capture(bool jpeg)
{
    esp_cam_ctlr_dvp_config_t cfg = {
        .ctlr_id = 0,
        .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = H_RES,
        .v_res = V_RES,
        .input_data_color_type = CAM_CTLR_COLOR_RGB565,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .dma_burst_size = 64,
        .pin = &s_pins,
        .bk_buffer_dis = 1,
        .pic_format_jpeg = jpeg,
        .xclk_freq = 20000000,
    };
    esp_cam_ctlr_handle_t cam = NULL;
    s_wrong_bars = jpeg ? NULL : wrong_bars_rgb565;
    ESP_ERROR_CHECK(esp_cam_new_dvp_ctlr(&cfg, &cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_get_frame_buffer_len(cam, &s_buf_len));
    /* External RAM, as IDF's own example: the driver syncs the buffer's cache,
     * and on S3 and S31 internal RAM has none to sync. */
    s_buf = esp_cam_ctlr_alloc_buffer(cam, s_buf_len, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        printf("no frame buffer of %u bytes\n", (unsigned)s_buf_len);
        return false;
    }
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(cam, &cbs, NULL));
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_start(cam));

    bool ok = true;
    for (int i = 0; i < FRAMES; i++) {
        frame_t f;
        if (xQueueReceive(s_done, &f, pdMS_TO_TICKS(10000)) != pdTRUE) {
            if (jpeg && i == 0) {
                /* Nobody showed the sensor a picture: it has none to send,
                 * which is how it should be, not a failure. */
                printf("JPEG: no picture shown, skipped\n");
                break;
            }
            printf("%s frame %d: timeout\n", jpeg ? "JPEG" : "DVP", i);
            ok = false;
            break;
        }
        if (jpeg) {
            printf("JPEG frame %d: size=%u crc=%08x\n", i, (unsigned)f.size, (unsigned)f.crc);
            ok = ok && f.size > 0;
            continue;
        }
        bool good = f.size == H_RES * V_RES * 2 && f.wrong == 0;
        printf("DVP frame %d: size=%u bars %s\n", i, (unsigned)f.size, good ? "ok" : "wrong");
        ok = ok && good;
    }
    ESP_ERROR_CHECK(esp_cam_ctlr_stop(cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_disable(cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_del(cam));
    heap_caps_free(s_buf);
    return ok;
}

#if SOC_MIPI_CSI_SUPPORTED
/* Bars wrong in a 320 x 240 GBRG RAW8 frame: green and blue on the middle
 * row, whose bar centres fall on even columns, red on the row after it. */
static int wrong_bars_gbrg(const uint8_t *buf)
{
    int wrong = 0;
    for (int i = 0; i < 8; i++) {
        int at = (V_RES / 2) * H_RES + (2 * i + 1) * H_RES / 16;
        if (buf[at] != BARS[i][1] || buf[at + 1] != BARS[i][2] || buf[at + H_RES] != BARS[i][0]) {
            wrong++;
        }
    }
    return wrong;
}

static bool sccb_write16(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t val)
{
    uint8_t w[3] = {reg >> 8, reg & 0xFF, val};
    return i2c_master_transmit(dev, w, sizeof(w), 100) == ESP_OK;
}

static bool sccb_read16(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t *val)
{
    uint8_t r[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(dev, r, sizeof(r), val, 1, 100) == ESP_OK;
}

/* Probe the OV5647 and program 320 x 240 RAW8: the MIPI bit mode and the
 * output size, as esp_cam_sensor's settings write them, the sensor asleep
 * until `stream` starts it. */
static i2c_master_dev_handle_t ov5647_setup(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OV5647_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_sccb, &cfg, &dev));
    uint8_t hi = 0, lo = 0;
    bool ok = sccb_read16(dev, 0x300A, &hi) && sccb_read16(dev, 0x300B, &lo);
    printf("OV5647: id=%02x%02x\n", hi, lo);
    if (!ok || hi != 0x56 || lo != 0x47) {
        return NULL;
    }
    static const struct {
        uint16_t reg;
        uint8_t val;
    } setting[] = {
        {0x0100, 0x00}, {0x3034, 0x18},
        {0x3808, H_RES >> 8}, {0x3809, H_RES & 0xFF}, {0x380A, V_RES >> 8}, {0x380B, V_RES & 0xFF},
    };
    for (size_t i = 0; i < sizeof(setting) / sizeof(setting[0]); i++) {
        ok = ok && sccb_write16(dev, setting[i].reg, setting[i].val);
    }
    return ok ? dev : NULL;
}

/* As IDF's test_csi_ov5647.c brings up the link: the MIPI PHY's LDO, then a
 * two-lane controller passing RAW8 through. */
static bool csi_capture(i2c_master_dev_handle_t sensor)
{
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {.chan_id = 3, .voltage_mv = 2500};
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));
    esp_cam_ctlr_csi_config_t cfg = {
        .ctlr_id = 0,
        .h_res = H_RES,
        .v_res = V_RES,
        .lane_bit_rate_mbps = 200,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,
        .data_lane_num = 2,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    esp_cam_ctlr_handle_t cam = NULL;
    s_wrong_bars = wrong_bars_gbrg;
    ESP_ERROR_CHECK(esp_cam_new_csi_ctlr(&cfg, &cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_get_frame_buffer_len(cam, &s_buf_len));
    s_buf = esp_cam_ctlr_alloc_buffer(cam, s_buf_len, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        printf("no frame buffer of %u bytes\n", (unsigned)s_buf_len);
        return false;
    }
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(cam, &cbs, NULL));
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam));
    bool ok = sccb_write16(sensor, 0x0100, 0x01);
    ESP_ERROR_CHECK(esp_cam_ctlr_start(cam));

    for (int i = 0; ok && i < FRAMES; i++) {
        frame_t f;
        if (xQueueReceive(s_done, &f, pdMS_TO_TICKS(5000)) != pdTRUE) {
            printf("CSI frame %d: timeout\n", i);
            ok = false;
            break;
        }
        bool good = f.size == H_RES * V_RES && f.wrong == 0;
        printf("CSI frame %d: size=%u bars %s\n", i, (unsigned)f.size, good ? "ok" : "wrong");
        ok = ok && good;
    }
    ESP_ERROR_CHECK(esp_cam_ctlr_stop(cam));
    sccb_write16(sensor, 0x0100, 0x00);
    ESP_ERROR_CHECK(esp_cam_ctlr_disable(cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_del(cam));
    heap_caps_free(s_buf);
    ESP_ERROR_CHECK(esp_ldo_release_channel(ldo));
    return ok;
}
#endif

void app_main(void)
{
    s_done = xQueueCreate(8, sizeof(frame_t));
    i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = SCCB_SDA,
        .scl_io_num = SCCB_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_sccb));
    bool ok = ov2640_setup() && dvp_capture(false);
    xQueueReset(s_done);
    ok = ov2640_jpeg() && dvp_capture(true) && ok;
#if SOC_MIPI_CSI_SUPPORTED
    xQueueReset(s_done);
    i2c_master_dev_handle_t ov5647 = ov5647_setup();
    ok = ov5647 && csi_capture(ov5647) && ok;
#endif
    printf("CAMERA_PART %s\n", ok ? "PASSED" : "FAILED");
}
