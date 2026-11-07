/* Quad-SPI writes the way real firmware makes them, with the stock drivers.
 *
 * The firmware side of the co-simulation SPI channel's framing. Two devices on
 * one quad bus (D, Q, WP and HD all routed), each a part:
 *
 *  - An esp_lcd QSPI panel IO, set up as the ST77916, CO5300 and SH8601 panel
 *    drivers set it up (quad_mode, 32-bit commands, 8-bit parameters).
 *    esp_lcd sends each command as a 4-byte opcode word (0x02 or 0x32, 0x00,
 *    lcd_cmd, 0x00) on one line with CS held, then the parameters on one line
 *    or the pixels on four, through DMA.
 *  - A flash-style device with an 8-bit command, a 24-bit address and dummy
 *    cycles, written with SPI_TRANS_MODE_QIO | SPI_TRANS_MULTILINE_ADDR.
 *
 * The test that runs this app reads what each part was sent from the
 * emulator's event log.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_err.h"

/* Pads legal on every target the test runs on, none of them a strapping pin
 * the boot needs. The test expects these as the pads on SIO0..SIO3. */
#define PIN_SCLK 6
#define PIN_D 4
#define PIN_Q 5
#define PIN_WP 1
#define PIN_HD 3
#define PIN_CS_LCD 7
#define PIN_CS_MEM 10

#define LCD_OPCODE_WRITE_CMD 0x02
#define LCD_OPCODE_WRITE_COLOR 0x32
#define PIXEL_BYTES 32

static volatile bool s_color_done;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    s_color_done = true;
    return false;
}

static int passed, failed;

static void check(const char *what, esp_err_t err)
{
    if (err == ESP_OK) {
        passed++;
    } else {
        printf("QSPI: %s err=%s\n", what, esp_err_to_name(err));
        failed++;
    }
}

void app_main(void)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SCLK,
        .data0_io_num = PIN_D,
        .data1_io_num = PIN_Q,
        .data2_io_num = PIN_WP,
        .data3_io_num = PIN_HD,
        .max_transfer_sz = 1024,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS_LCD,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 2,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = { .quad_mode = 1 },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    /* COLMOD = 0x55 (RGB565). */
    uint8_t colmod = 0x55;
    check("colmod", esp_lcd_panel_io_tx_param(io, (LCD_OPCODE_WRITE_CMD << 24) | (0x3A << 8), &colmod, 1));

    uint8_t *px = heap_caps_malloc(PIXEL_BYTES, MALLOC_CAP_DMA);
    for (int i = 0; i < PIXEL_BYTES; i++) {
        px[i] = (uint8_t)(0xA0 + i);
    }
    check("ramwr", esp_lcd_panel_io_tx_color(io, (LCD_OPCODE_WRITE_COLOR << 24) | (0x2C << 8), px, PIXEL_BYTES));
    for (int i = 0; i < 1000 && !s_color_done; i++) {
        vTaskDelay(1);
    }
    check("color done", s_color_done ? ESP_OK : ESP_ERR_TIMEOUT);

    /* A flash-style QIO page program: 0x32, a 24-bit address on four lines. */
    spi_device_interface_config_t mem_cfg = {
        .command_bits = 8,
        .address_bits = 24,
        .dummy_bits = 4,
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_CS_MEM,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    spi_device_handle_t mem;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &mem_cfg, &mem));
    spi_transaction_t t = {
        .flags = SPI_TRANS_MODE_QIO | SPI_TRANS_MULTILINE_ADDR | SPI_TRANS_USE_TXDATA,
        .cmd = 0x32,
        .addr = 0x123456,
        .length = 32,
        .tx_data = { 0xDE, 0xAD, 0xBE, 0xEF },
    };
    check("page program", spi_device_polling_transmit(mem, &t));

    printf("QSPI: passed=%d failed=%d\n", passed, failed);
    printf("SPI_QSPI: done\n");
    fflush(stdout);
}
