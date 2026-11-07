/* A TI TMP121/TMP123 read through the stock esp_driver_spi, on every GPSPI bus.
 *
 * The firmware side of the co-simulation SPI channel: a part is the sensor, and
 * nothing here knows it is virtual. The sensor has no data input. With its chip
 * select low it clocks out 16 bits, most significant first: bits 15..3 are the
 * temperature in 13-bit two's complement at 0.0625 C a count, bit 2 reads 0
 * and bits 1..0 are not driven (SBOS273). SPI2 has it on MISO 2, SCLK 6 and
 * CS 7; SPI3, on the chips that have one, on MISO 4, SCLK 5 and CS 15.
 *
 * Per bus it reads the sensor three times: with spi_device_polling_transmit,
 * which spins on the transfer's completion, with spi_device_transmit, which
 * sleeps on the driver's semaphore until its interrupt, and with
 * spi_device_transmit again after reopening the bus with SPI_DMA_CH_AUTO, the
 * setup most drivers use, where the read lands in its buffer through the GDMA.
 * After `SPI_TMP123: done` it reads SPI2's sensor once a second, through DMA,
 * so a change made to the part shows in the output.
 */
#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

static const struct {
    spi_host_device_t host;
    int miso, sclk, cs;
} BUSES[] = {
    { SPI2_HOST, 2, 6, 7 },
#if SOC_SPI_PERIPH_NUM > 2
    { SPI3_HOST, 4, 5, 15 },
#endif
};
#define NUM_BUSES (sizeof BUSES / sizeof BUSES[0])

/* Degrees C from the 16-bit word: the three low bits out, the 13-bit field
 * sign-extended by the arithmetic shift, then scaled. */
static float celsius(uint16_t raw)
{
    return (float)((int16_t)raw >> 3) * 0.0625f;
}

static esp_err_t read_sensor(spi_device_handle_t dev, bool polling, uint16_t *raw)
{
    uint8_t rx[2] = { 0 };
    spi_transaction_t t = {
        .length = 16,
        .rxlength = 16,
        .rx_buffer = rx,
    };
    esp_err_t err = polling ? spi_device_polling_transmit(dev, &t) : spi_device_transmit(dev, &t);
    *raw = (uint16_t)((rx[0] << 8) | rx[1]);
    return err;
}

static void report(int host, const char *how, esp_err_t err, uint16_t raw)
{
    if (err != ESP_OK) {
        printf("TMP123: host=%d %s err=%s\n", host, how, esp_err_to_name(err));
    } else {
        printf("TMP123: host=%d %s raw=0x%04" PRIX16 " t=%.4f\n", host, how, raw, celsius(raw));
    }
}

static spi_device_handle_t open_bus(size_t i, spi_dma_chan_t dma)
{
    spi_bus_config_t bus_cfg = {
        .miso_io_num = BUSES[i].miso,
        .mosi_io_num = -1,
        .sclk_io_num = BUSES[i].sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BUSES[i].host, &bus_cfg, dma));
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 1000 * 1000,
        .mode = 0,
        .spics_io_num = BUSES[i].cs,
        .queue_size = 1,
    };
    spi_device_handle_t dev;
    ESP_ERROR_CHECK(spi_bus_add_device(BUSES[i].host, &dev_cfg, &dev));
    return dev;
}

void app_main(void)
{
    spi_device_handle_t sensors[NUM_BUSES];
    for (size_t i = 0; i < NUM_BUSES; i++) {
        sensors[i] = open_bus(i, SPI_DMA_DISABLED);

        /* SPI2_HOST is 1 and SPI3_HOST 2 in spi_host_device_t; the bus is
         * printed by its GPSPI number. */
        int host = BUSES[i].host + 1;
        uint16_t raw;
        esp_err_t err = read_sensor(sensors[i], true, &raw);
        report(host, "polling", err, raw);
        err = read_sensor(sensors[i], false, &raw);
        report(host, "interrupt", err, raw);

        ESP_ERROR_CHECK(spi_bus_remove_device(sensors[i]));
        ESP_ERROR_CHECK(spi_bus_free(BUSES[i].host));
        sensors[i] = open_bus(i, SPI_DMA_CH_AUTO);
        err = read_sensor(sensors[i], false, &raw);
        report(host, "dma", err, raw);
    }
    printf("SPI_TMP123: done\n");
    fflush(stdout);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint16_t raw;
        esp_err_t err = read_sensor(sensors[0], false, &raw);
        if (err == ESP_OK) {
            printf("THERMO: t=%.4f\n", celsius(raw));
        } else {
            printf("THERMO: err=%s\n", esp_err_to_name(err));
        }
        fflush(stdout);
    }
}
