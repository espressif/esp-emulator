/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The hardware JPEG encoder through the stock esp_driver_jpeg: a smooth
 * picture in each input format, encoded, then decoded again by the mask
 * ROM's TJpgDec and held against the picture it started as.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/jpeg_encode.h"
#include "esp_err.h"
#include "esp32s31/rom/tjpgd.h"

#define W 160
#define H 120
/* Mean absolute error per channel a quality-90 picture stays well inside. */
#define MAE_LIMIT_X100 600

static uint8_t s_rgb[W * H * 3];
static uint8_t s_work[4096];

typedef struct {
    const uint8_t *jpg;
    size_t len;
    size_t at;
    uint64_t err;
    uint32_t count;
} decode_t;

static UINT read_jpg(JDEC *jd, BYTE *buf, UINT n)
{
    decode_t *d = jd->device;
    if (n > d->len - d->at) {
        n = d->len - d->at;
    }
    if (buf) {
        memcpy(buf, d->jpg + d->at, n);
    }
    d->at += n;
    return n;
}

static uint8_t luma(const uint8_t *p)
{
    return (uint8_t)((299 * p[0] + 587 * p[1] + 114 * p[2] + 500) / 1000);
}

/* A block of decoded RGB888: add its distance from the source picture. */
static UINT take_block(JDEC *jd, void *bitmap, JRECT *r)
{
    decode_t *d = jd->device;
    const uint8_t *px = bitmap;
    for (int y = r->top; y <= r->bottom; y++) {
        for (int x = r->left; x <= r->right; x++, px += 3) {
            if (x >= W || y >= H) {
                continue;
            }
            const uint8_t *want = &s_rgb[(y * W + x) * 3];
            for (int c = 0; c < 3; c++) {
                d->err += abs((int)px[c] - (int)want[c]);
            }
            d->count += 3;
        }
    }
    return 1;
}

static void ycc(const uint8_t *p, uint8_t *y, uint8_t *u, uint8_t *v)
{
    int r = p[0], g = p[1], b = p[2];
    *y = luma(p);
    *u = (uint8_t)(128 + (-168736 * r - 331264 * g + 500000 * b) / 1000000);
    *v = (uint8_t)(128 + (500000 * r - 418688 * g - 81312 * b) / 1000000);
}

/* The picture in the encoder's input format; returns its size. */
static size_t fill(jpeg_enc_input_format_t fmt, uint8_t *out)
{
    size_t n = 0;
    for (int i = 0; i < W * H; i++) {
        const uint8_t *p = &s_rgb[i * 3];
        uint8_t y, u, v;
        ycc(p, &y, &u, &v);
        switch (fmt) {
        case JPEG_ENCODE_IN_FORMAT_RGB888:
            out[n++] = p[2];
            out[n++] = p[1];
            out[n++] = p[0];
            break;
        case JPEG_ENCODE_IN_FORMAT_RGB565: {
            uint16_t w = ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3);
            out[n++] = w & 0xFF;
            out[n++] = w >> 8;
            break;
        }
        case JPEG_ENCODE_IN_FORMAT_GRAY:
            out[n++] = y;
            break;
        default: /* YUV422: U Y V Y for each pair */
            out[n++] = (i & 1) ? v : u;
            out[n++] = y;
            break;
        }
    }
    return n;
}

static bool one(jpeg_encoder_handle_t enc, const char *name, jpeg_enc_input_format_t fmt,
                jpeg_down_sampling_type_t sub, uint8_t *in, uint8_t *out, size_t out_cap)
{
    size_t in_len = fill(fmt, in);
    jpeg_encode_cfg_t cfg = {
        .width = W,
        .height = H,
        .src_type = fmt,
        .sub_sample = sub,
        .image_quality = 90,
    };
    uint32_t size = 0;
    esp_err_t err = jpeg_encoder_process(enc, &cfg, in, in_len, out, out_cap, &size);
    if (err != ESP_OK) {
        printf("JPEG %s: encode failed: %s\n", name, esp_err_to_name(err));
        return false;
    }
    if (fmt == JPEG_ENCODE_IN_FORMAT_GRAY) {
        /* The ROM's TJpgDec takes no single-component picture: the markers only. */
        bool framed = size > 4 && out[0] == 0xFF && out[1] == 0xD8 && out[size - 2] == 0xFF && out[size - 1] == 0xD9;
        printf("JPEG %s: size=%u %s\n", name, (unsigned)size, framed ? "framed" : "not framed");
        return framed;
    }
    decode_t d = {.jpg = out, .len = size};
    JDEC jd;
    JRESULT res = jd_prepare(&jd, read_jpg, s_work, sizeof(s_work), &d);
    if (res == JDR_OK) {
        res = jd_decomp(&jd, take_block, 0);
    }
    if (res != JDR_OK || jd.width != W || jd.height != H || d.count != W * H * 3) {
        printf("JPEG %s: size=%u decode failed: res=%d %ux%u, %u samples\n", name, (unsigned)size, res,
               jd.width, jd.height, (unsigned)d.count);
        return false;
    }
    unsigned mae = (unsigned)(d.err * 100 / d.count);
    printf("JPEG %s: size=%u mae=%u.%02u\n", name, (unsigned)size, mae / 100, mae % 100);
    return mae < MAE_LIMIT_X100;
}

void app_main(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *p = &s_rgb[(y * W + x) * 3];
            p[0] = x * 255 / (W - 1);
            p[1] = y * 255 / (H - 1);
            p[2] = (x + y) * 255 / (W + H - 2);
        }
    }
    jpeg_encoder_handle_t enc = NULL;
    jpeg_encode_engine_cfg_t eng = {.timeout_ms = 2000};
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&eng, &enc));

    jpeg_encode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
    jpeg_encode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    size_t in_cap = 0, out_cap = 0;
    uint8_t *in = jpeg_alloc_encoder_mem(W * H * 3, &in_cfg, &in_cap);
    uint8_t *out = jpeg_alloc_encoder_mem(W * H * 3, &out_cfg, &out_cap);
    bool ok = in && out;

    ok = ok && one(enc, "yuv422", JPEG_ENCODE_IN_FORMAT_YUV422, JPEG_DOWN_SAMPLING_YUV422, in, out, out_cap);
    ok = ok && one(enc, "rgb565", JPEG_ENCODE_IN_FORMAT_RGB565, JPEG_DOWN_SAMPLING_YUV420, in, out, out_cap);
    ok = ok && one(enc, "rgb888", JPEG_ENCODE_IN_FORMAT_RGB888, JPEG_DOWN_SAMPLING_YUV444, in, out, out_cap);
    ok = ok && one(enc, "gray", JPEG_ENCODE_IN_FORMAT_GRAY, JPEG_DOWN_SAMPLING_GRAY, in, out, out_cap);

    jpeg_del_encoder_engine(enc);
    printf("JPEG_ENCODE %s\n", ok ? "PASSED" : "FAILED");
}
