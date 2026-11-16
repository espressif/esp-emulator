# `test_apps/` — ESP-IDF firmware the emulator is tested against

Each directory is a real ESP-IDF project that esp-emu boots through the full
chain — mask ROM → 2nd-stage bootloader → app — on every supported chip.

| App | What it exercises | Chips |
|---|---|---|
| `smoke/` | system info, NVS, flash, mmap, timers, FreeRTOS, pthread, esp_event, GPIO/RMT/LEDC/PCNT/I2C/I2S, SD card blocks written and read back (SDMMC host on P4, S3 and S31, SPI mode elsewhere), crypto (PSA, SHA/AES/HMAC/ECDSA), SMP, WiFi + DHCP, Ethernet + DHCP, BLE advertising, reset reasons, task watchdog, PIE against recorded silicon results (P4, S31), ESP32-S3 `ee.*` SIMD instructions against recorded silicon results | all |
| `http_throughput/` | HTTP download throughput over WiFi (Ethernet on P4) | c3, c5, c6, p4, s3, s31 |
| `tee/` | ESP-TEE: secure storage, attestation, TEE/REE isolation | c5, c6, h2 |
| `flash_enc/` | flash encryption: key generation, first-boot encryption, encrypted reads | all |
| `flash32/` | 32 MiB flash (`--flash-size`) | c3, c5, p4, s31 |
| `psram/` | external RAM (`--psram-size`) | p4, s31 |
| `coremark/` | CoreMark benchmark: emulation speed per chip | all |
| `wpa3_sta/` | WPA3-SAE station against the built-in soft AP, repeated init cycles | c3, c5, c6, s3 |
| `unmapped_probe/` | loads and stores outside ordinary memory: which ones fault, which read 0, as measured on an ESP32-S3 board (`expected_esp32s3.txt`) | s3 |
| `usb_cdc/` | TinyUSB CDC-ACM console on every USB-OTG port: the virtual USB host enumerates the device, the app's output arrives over the CDC port, and a typed line echoes back (`--console usb-cdc`) | s3, p4, s31 |
| `gpio_button/` | a GPIO input with pull-up and an interrupt that prints every edge; the firmware of the [button board](../examples/boards/button/README.md) and the browser button demo | c3 |
| `i2c_sensor/` | reads an SHT31 on every I2C port with the stock `esp_driver_i2c`; the firmware of the [sensor board](../examples/boards/sensor/README.md) and the browser sensor demo | c3, s3, p4 |
| `spi_tmp123/` | reads a TMP123 on every SPI bus with the stock `esp_driver_spi`, polling, by interrupt and through DMA; the firmware of the [thermometer board](../examples/boards/thermometer/README.md) | c3, s3, p4, s31 |
| `spi_qspi/` | quad-SPI writes as real firmware makes them: an `esp_lcd` QSPI panel and a flash-style command/address/dummy device on one bus, each a virtual part | c3, c5, s3, p4 |
| `cosim_high_pads/` | virtual parts on pads above GPIO 31: an I2C device on each port, a button and an LED, through the stock `esp_driver_i2c` and `gpio` drivers | p4, s3, s31 |
| `uart_mhz19b/` | reads an MH-Z19B CO2 sensor on UART1 with the stock UART driver; the firmware of the [CO2 board](../examples/boards/co2/README.md) and the browser CO2 demo | c3, s3, p4 |
| `pcnt_encoder/` | counts a KY-040 rotary encoder with the stock `esp_driver_pcnt`, set up as ESP-IDF's `rotary_encoder` example; the firmware of the [encoder board](../examples/boards/encoder/README.md) and the browser encoder demo | c5, c6, h2, p4, s3, s31 |
| `pwm_servo/` | drives an SG90 servo with LEDC, and with MCPWM where the chip has it; the firmware of the [servo board](../examples/boards/servo/README.md) and the browser servo demo | all |
| `adc_oneshot/` | reads a voltage a part holds on an ADC pin with `adc_oneshot_read()`; the firmware of the [analog board](../examples/boards/analog/README.md) (a potentiometer and an NTC thermistor) and the browser ADC demo | c3, c5, c6, h2, s31 |
| `camera_dvp/` | the stock `esp_driver_cam` DVP controller receives raw and JPEG frames from a camera sensor played over `--cosim-tcp`; the frame buffer is in external RAM | p4, s3, s31 |
| `jpeg_encode/` | the stock `esp_driver_jpeg` encoder on a picture in four input formats; each colour result is decoded again by the mask ROM's decoder and compared with the picture | s31 |
| `en_watchdog/` | kicks a windowed watchdog supervisor wired to the chip's EN pin ten times, then hangs, so the part resets the chip; the firmware of the [watchdog board](../examples/boards/watchdog/README.md) | c3, c6, p4, s3 |

## Running the smoke app

`run_smoke.sh` builds `smoke/` per chip and runs it in the `esp-emu` on your
`PATH`. It passes when at least one chip actually ran and every run's final
`RESULT_SUMMARY:` line reports no failures. Builds use the
`espressif/idf:latest` docker image by default; pass `--idf-path` to use a
sourced ESP-IDF checkout instead.

An unknown `--chips` name is rejected as a usage error (exit 2). A chip the
chosen IDF cannot target is reported as `SKIP`; when every requested chip is
skipped, the script fails (exit 1), so a typo cannot look like a pass.

If the IDF backend cannot be reached, the script prints the cause and exits 2.
This covers docker missing, the docker daemon not running or not permitted,
and an `--idf-path` directory without `export.sh`.

```sh
./install.sh                                   # esp-emu 0.46 or newer
test_apps/run_smoke.sh                         # every chip this IDF can build
test_apps/run_smoke.sh --chips esp32c3,esp32c6 # a subset
test_apps/run_smoke.sh --idf-path "$IDF_PATH"  # local ESP-IDF
```

Build output and the emulator log for each chip land under `./out/smoke-<chip>/`
(`--out DIR` to change). A chip the chosen IDF cannot target is reported as
`SKIP`. `.github/workflows/release-smoke.yml` runs the same script against
every published release with ESP-IDF master.

Per-chip pass counts differ because some cases only exist on some chips (BLE,
WiFi, Ethernet, SMP); a count that differs *between* chips is expected, a
chip's own count dropping is not.

The `sd_card` case writes a few blocks of an SD card and reads them back
through the stock driver: the SDMMC host on ESP32-P4, S3 and S31, SPI mode on
SPI2 with chip select GPIO 3 on the other chips. With no card it says so and
passes, so the app still runs on a board without one. `run_smoke.sh` gives it
a blank 8 MiB card with `--sd` when the `esp-emu` it runs has the flag
(1.1.0 or newer), and then fails a chip whose case did not reach the card.

## Building an app by hand

```sh
cp -r test_apps/smoke /tmp/smoke && cd /tmp/smoke
idf.py --preview set-target esp32c6      # --preview is needed for esp32s31, harmless otherwise
idf.py build && idf.py merge-bin -o build/merged.bin
esp-emu --chip esp32c6 --firmware build/merged.bin --elf build/emu_test.elf \
  --no-panic-intercept --timeout 240s --exit-on "RESULT_SUMMARY:"
```

`--elf` enables BLE interception and symbolicated backtraces;
`--no-panic-intercept` lets the app's deliberate task-watchdog panic reboot
the chip the way the test expects.
