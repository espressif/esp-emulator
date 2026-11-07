/* Co-simulated parts on pads above GPIO 31, through the stock esp_driver_i2c
 * and gpio drivers.
 *
 * Bank 1 of the GPIO block (GPIO 32 and up) is where real boards put their
 * buses: the M5Stack Tab5 (ESP32-P4) runs its I2C bus on SDA 31 / SCL 32, and
 * ESP-Mosaico (ESP32-S31) on SDA 56 / SCL 3 with its boot button on GPIO 61.
 * Each chip here wires one I2C port across the bank boundary and the other
 * entirely above it, a button on a high pad, and an LED on another.
 *
 * Per port it scans the bus and reads one register from the device there.
 * Then it waits for the button to read low, lights the LED, and waits for the
 * button to read high: the part holding the button copies the LED pad onto
 * it, so the release proves the LED level left the chip on a bank-1 pad and
 * the press and release both reached gpio_get_level on another.
 *
 * ESP32-P4, S3 and S31: the targets with more than 32 GPIOs. Every line is
 * PROBE:<what> so the harness can key on it.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4
/* Port 0 as the M5Stack Tab5 wires it, straddling banks 0 and 1; port 1
 * entirely in bank 1. */
#define I2C0_SCL 32
#define I2C0_SDA 31
#define I2C1_SCL 47
#define I2C1_SDA 48
#define BTN_PIN  45
#define LED_PIN  46
#elif CONFIG_IDF_TARGET_ESP32S31
/* Port 0 as ESP-Mosaico wires it, and its boot button. */
#define I2C0_SCL  3
#define I2C0_SDA 56
#define I2C1_SCL 57
#define I2C1_SDA 58
#define BTN_PIN  61
#define LED_PIN  60
#elif CONFIG_IDF_TARGET_ESP32S3
/* 26-37 are the flash / PSRAM pads on S3 modules, so the high pads start at 38. */
#define I2C0_SCL 38
#define I2C0_SDA 39
#define I2C1_SCL 40
#define I2C1_SDA 41
#define BTN_PIN  47
#define LED_PIN  48
#else
#error "only chips with more than 32 GPIOs have pads above GPIO 31"
#endif

static i2c_master_bus_handle_t new_bus(i2c_port_num_t port, int scl, int sda)
{
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .scl_io_num = scl,
        .sda_io_num = sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    return bus;
}

/* Every 7-bit address but the reserved ones. */
static void scan(const char *what, i2c_master_bus_handle_t bus)
{
    char found[96] = "";
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
            continue;
        }
        snprintf(found + strlen(found), sizeof found - strlen(found), " 0x%02X", addr);
    }
    printf("PROBE:%s scan%s\n", what, found[0] ? found : " none");
}

/* One register, written then read in one transaction: the bytes crossed, not
 * only an ACK. */
static void read_reg(const char *what, i2c_master_bus_handle_t bus, uint8_t addr, uint8_t reg)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &dev));
    uint8_t val = 0;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 1000);
    if (err != ESP_OK) {
        printf("PROBE:%s addr=0x%02X err=%s\n", what, addr, esp_err_to_name(err));
        return;
    }
    printf("PROBE:%s addr=0x%02X reg=0x%02X val=0x%02X\n", what, addr, reg, val);
}

void app_main(void)
{
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << BTN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    i2c_master_bus_handle_t bus0 = new_bus(I2C_NUM_0, I2C0_SCL, I2C0_SDA);
    i2c_master_bus_handle_t bus1 = new_bus(I2C_NUM_1, I2C1_SCL, I2C1_SDA);
    printf("PROBE:pads i2c0=%d/%d i2c1=%d/%d btn=%d led=%d\n", I2C0_SCL, I2C0_SDA, I2C1_SCL,
           I2C1_SDA, BTN_PIN, LED_PIN);

    scan("i2c0", bus0);
    scan("i2c1", bus1);
    read_reg("touch", bus0, 0x14, 0x81);
    read_reg("imu", bus1, 0x68, 0x75);

    /* Released, the pin reads high through the pull-up. A bank-1 IN register
     * that is not modelled reads 0: the ready line says so, and the wait for
     * the release below never ends. */
    printf("PROBE:button ready level=%d\n", gpio_get_level(BTN_PIN));
    fflush(stdout);
    while (gpio_get_level(BTN_PIN)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("PROBE:button pressed\n");
    fflush(stdout);

    /* Low long enough for the part to see it, then high: the rising edge is
     * what the part copies onto the button. */
    gpio_config_t out = { .pin_bit_mask = 1ULL << LED_PIN, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(LED_PIN, 1);
    while (!gpio_get_level(BTN_PIN)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("PROBE:button released by led=%d\n", LED_PIN);

    printf("PROBE:done\n");
    fflush(stdout);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
