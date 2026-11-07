/* An SHT31 read through the stock esp_driver_i2c, on every HP I2C port.
 *
 * The firmware side of the co-simulation I2C channel: a part answers for the
 * sensor, and nothing here knows it is virtual. Port 0 is on SCL 6 / SDA 5,
 * except on ESP32-P4, where it is on the M5Stack Tab5's I2C pads, SCL 32 /
 * SDA 31, in bank 1 of the GPIO matrix. Port 1 is on SCL 10 / SDA 11.
 *
 * Per port it scans the bus, then reads a measurement twice: as a command and a
 * read in two transactions (i2c_master_transmit, i2c_master_receive), and as
 * one (i2c_master_transmit_receive), with a repeated START between the command
 * and the read. If a memory answers at 0x50 on port 0, it writes 40 bytes to it
 * and reads them back: more than the controller's 32-byte FIFO, so the driver
 * sends each in pieces. After `I2C_SENSOR: done` it reads port 0's sensor once
 * a second, so a change made to the part shows in the output.
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

#define SHT31_ADDR 0x44
/* A memory whose first written byte is the address it reads and writes from.
 * A real 24C02 wraps a write at its 8-byte page, so this test needs one that
 * does not, as a test part provides. */
#define MEMORY_ADDR 0x50
#define LONG_LEN 40

static const struct {
    int scl, sda;
#if CONFIG_IDF_TARGET_ESP32P4
} PADS[] = { { 32, 31 }, { 10, 11 } };
#else
} PADS[] = { { 6, 5 }, { 10, 11 } };
#endif

/* CRC-8, polynomial 0x31, initial value 0xFF (SHT3x datasheet 4.12). */
static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static float temperature(const uint8_t *b)
{
    return -45.0f + 175.0f * (float)((b[0] << 8) | b[1]) / 65535.0f;
}

static float humidity(const uint8_t *b)
{
    return 100.0f * (float)((b[3] << 8) | b[4]) / 65535.0f;
}

static void report(int port, const char *how, esp_err_t err, const uint8_t *b)
{
    if (err != ESP_OK) {
        printf("SHT31: port=%d %s err=%s\n", port, how, esp_err_to_name(err));
        return;
    }
    bool crc_ok = crc8(b, 2) == b[2] && crc8(b + 3, 2) == b[5];
    printf("SHT31: port=%d %s t=%.2f rh=%.2f crc=%s\n", port, how, temperature(b), humidity(b),
           crc_ok ? "ok" : "bad");
}

/* Single shot, high repeatability, no clock stretching: the command, the
 * 15 ms measurement, then the read. */
static esp_err_t measure(i2c_master_dev_handle_t dev, uint8_t *buf)
{
    const uint8_t cmd[] = { 0x24, 0x00 };
    esp_err_t err = i2c_master_transmit(dev, cmd, sizeof cmd, 100);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (err == ESP_OK) {
        err = i2c_master_receive(dev, buf, 6, 100);
    }
    return err;
}

/* A write and a read of LONG_LEN bytes at memory address 0x10. Silent when
 * nothing answers at MEMORY_ADDR, as on a board with the sensor alone. */
static void long_transfers(int port, i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, MEMORY_ADDR, 50) != ESP_OK) {
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MEMORY_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &dev));

    uint8_t out[1 + LONG_LEN] = { 0x10 };
    for (int i = 0; i < LONG_LEN; i++) {
        out[1 + i] = (uint8_t)(0xA0 ^ (i * 7));
    }
    uint8_t in[LONG_LEN] = { 0 };
    esp_err_t err = i2c_master_transmit(dev, out, sizeof out, 100);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(dev, out, 1, in, sizeof in, 100);
    }
    if (err != ESP_OK) {
        printf("MEMORY: port=%d err=%s\n", port, esp_err_to_name(err));
    } else {
        printf("MEMORY: port=%d wrote=%d read=%d %s\n", port, LONG_LEN, LONG_LEN,
               memcmp(out + 1, in, LONG_LEN) == 0 ? "match" : "mismatch");
    }
    ESP_ERROR_CHECK(i2c_master_bus_rm_device(dev));
}

void app_main(void)
{
    i2c_master_dev_handle_t sensors[SOC_HP_I2C_NUM];
    for (int port = 0; port < SOC_HP_I2C_NUM; port++) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = port,
            .scl_io_num = PADS[port].scl,
            .sda_io_num = PADS[port].sda,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        i2c_master_bus_handle_t bus;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

        /* Every 7-bit address but the reserved ones: a modelled board answers
         * at its parts' addresses and nowhere else. */
        printf("SCAN: port=%d found=", port);
        for (uint16_t addr = 0x08; addr < 0x78; addr++) {
            if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
                printf("0x%02X ", addr);
            }
        }
        printf("\n");

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = SHT31_ADDR,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));
        sensors[port] = dev;

        uint8_t buf[6];
        report(port, "two", measure(dev, buf), buf);

        /* With clock stretching: the command and the read in one transaction. */
        const uint8_t stretch[] = { 0x2C, 0x06 };
        esp_err_t err = i2c_master_transmit_receive(dev, stretch, sizeof stretch, buf, sizeof buf, 100);
        report(port, "one", err, buf);

        if (port == 0) {
            long_transfers(port, bus);
        }
    }
    printf("I2C_SENSOR: done\n");
    fflush(stdout);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint8_t buf[6];
        esp_err_t err = measure(sensors[0], buf);
        if (err == ESP_OK) {
            printf("SENSOR: t=%.2f rh=%.2f\n", temperature(buf), humidity(buf));
        } else {
            printf("SENSOR: err=%s\n", esp_err_to_name(err));
        }
        fflush(stdout);
    }
}
