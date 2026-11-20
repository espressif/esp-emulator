/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A button and an SSD1306, both virtual.
 *
 * Press the button and the display says so. Nothing here knows either part is
 * a model: the I2C driver is the stock one, the interrupt is a real interrupt,
 * and the command bytes are the ones the datasheet asks for. The display is
 * on I2C port 0, SCL 6 / SDA 5, except on ESP32-P4, where it is on SCL 32 /
 * SDA 31 as in test_apps/i2c_sensor; the button is on GPIO 9.
 *
 * Deliberately hand-rolled rather than using a display component. The point is
 * to exercise the command stream a device model has to decode — the init
 * sequence with its one- and two-byte parameters, the addressing window, and a
 * full-frame push — and a component would hide exactly that behind an API.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#define BUTTON_GPIO 9
#if CONFIG_IDF_TARGET_ESP32P4
#define I2C_SDA 31
#define I2C_SCL 32
#else
#define I2C_SDA 5
#define I2C_SCL 6
#endif
#define OLED_ADDR 0x3C

#define OLED_W 128
#define OLED_H 64
#define OLED_PAGES (OLED_H / 8)

/* GDDRAM layout: one byte is eight vertically stacked pixels, bit 0 on top. */
static uint8_t fb[OLED_PAGES][OLED_W];
static i2c_master_dev_handle_t oled;
static QueueHandle_t edges;

/* 5x7 glyphs, column-major, for the characters this app prints. Column-major
 * because that is how they land in GDDRAM — one byte per column, straight in. */
static const uint8_t FONT[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
};

static void oled_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[40];
    buf[0] = 0x00; /* control: a stream of commands follows */
    memcpy(buf + 1, cmds, n);
    ESP_ERROR_CHECK(i2c_master_transmit(oled, buf, n + 1, 1000));
}

static void oled_init(void)
{
    /* The standard bring-up. Every command with a parameter sends it in the
     * same transfer, so a model that miscounts them loses sync here rather
     * than somewhere subtle later. */
    static const uint8_t init[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock divide / oscillator */
        0xA8, 0x3F,       /* multiplex ratio: 64 rows */
        0xD3, 0x00,       /* display offset */
        0x40,             /* start line 0 */
        0x8D, 0x14,       /* charge pump on */
        0x20, 0x00,       /* horizontal addressing */
        0xA1, 0xC8,       /* segment remap, COM scan direction */
        0xDA, 0x12,       /* COM pin configuration */
        0x81, 0xCF,       /* contrast */
        0xD9, 0xF1,       /* pre-charge */
        0xDB, 0x40,       /* VCOMH deselect */
        0xA4,             /* resume from RAM */
        0xA6,             /* not inverted */
        0xAF,             /* display on */
    };
    oled_cmds(init, sizeof init);
}

/* Push the whole framebuffer. The window is set once and 1024 bytes follow;
 * horizontal addressing walks the columns and wraps down the pages. */
static void oled_flush(void)
{
    static const uint8_t window[] = { 0x21, 0, OLED_W - 1, 0x22, 0, OLED_PAGES - 1 };
    oled_cmds(window, sizeof window);

    static uint8_t buf[1 + OLED_PAGES * OLED_W];
    buf[0] = 0x40; /* control: a stream of data follows */
    memcpy(buf + 1, fb, OLED_PAGES * OLED_W);
    ESP_ERROR_CHECK(i2c_master_transmit(oled, buf, sizeof buf, 2000));
}

static void draw_text(int x, int page, const char *s)
{
    for (; *s && x + 6 <= OLED_W; s++, x += 6) {
        int idx = (*s == ' ') ? 0 : (*s >= 'A' && *s <= 'Z') ? *s - 'A' + 1 : 0;
        for (int c = 0; c < 5; c++) {
            fb[page][x + c] = FONT[idx][c];
        }
        fb[page][x + 5] = 0x00; /* the gap between characters */
    }
}

static void show(const char *line1, const char *line2)
{
    memset(fb, 0, sizeof fb);
    draw_text(4, 2, line1);
    draw_text(4, 4, line2);
    oled_flush();
}

static void IRAM_ATTR on_edge(void *arg)
{
    int level = gpio_get_level(BUTTON_GPIO);
    xQueueSendFromISR(edges, &level, NULL);
}

void app_main(void)
{
    edges = xQueueCreate(8, sizeof(int));

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = I2C_SCL,
        .sda_io_num = I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &oled));

    oled_init();
    show("HELLO", "READY");
    printf("OLED: ready\n");
    fflush(stdout);

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
    printf("BUTTON: ready level=%d\n", gpio_get_level(BUTTON_GPIO));
    fflush(stdout);

    int level;
    for (;;) {
        if (xQueueReceive(edges, &level, portMAX_DELAY) == pdTRUE) {
            /* Active low: the contact shorts the pin to ground. */
            show("BUTTON", level ? "RELEASED" : "PRESSED");
            printf("BUTTON: %s\n", level ? "released" : "pressed");
            fflush(stdout);
        }
    }
}
