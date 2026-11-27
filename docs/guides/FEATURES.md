# What esp-emu Emulates

This page lists what works on each chip and what is not emulated yet. To install and run the emulator, see [GETTING-STARTED.md](GETTING-STARTED.md).

## Support matrix

<!-- chip-matrix:begin -->
✅ works &nbsp;·&nbsp; ⏳ on the chip, not emulated yet &nbsp;·&nbsp; — not on this chip

| | ESP32 | C3 | C5 | C6 | H2 | P4 | S3 | S31 |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| CPU | Xtensa LX6 ×2 | RV32IMC | RV32IMAC | RV32IMAC + LP core | RV32IMAC | RV32IMAFC ×2 | Xtensa LX7 ×2 | RV32IMAFC ×2 |
| **Wireless and network** | | | | | | | | |
| Wi-Fi (2.4 GHz) | ✅ | ✅ | ✅ | ✅ | — | — | ✅ | ✅ |
| Bluetooth LE | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ | ✅ |
| Thread / 802.15.4 | — | — | ⏳ | ✅ | ✅ | — | — | ⏳ |
| Ethernet | ✅ | — | — | — | — | ✅ | — | ✅ |
| **Buses and storage** | | | | | | | | |
| UART · I2C · GPIO · timers · watchdogs | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| SPI master | ⏳ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| USB Serial/JTAG | — | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| USB-OTG (device) | — | — | — | — | — | ✅ | ✅ | ✅ |
| SD card (SDMMC host) | ✅ | — | — | — | — | ✅ | ✅ | ✅ |
| External RAM (PSRAM) | ✅ | — | ✅ | — | — | ✅ | ✅ | ✅ |
| **Timing and analog** | | | | | | | | |
| LEDC (PWM) | ⏳ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| RMT | ⏳ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| PCNT | ⏳ | — | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| MCPWM | ⏳ | — | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| I2S | ⏳ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| ADC | ⏳ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Event Task Matrix | — | — | ✅ | ✅ | ✅ | ✅ | — | ✅ |
| TWAI (CAN) | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| **Display and camera** | | | | | | | | |
| Camera (DVP) | — | — | — | — | — | ✅ | ✅ | ✅ |
| LCD (i80, RGB) | ⏳ | — | — | — | — | ✅ | ✅ | ✅ |
| JPEG · PPA | — | — | — | — | — | ✅ | — | ✅ |
| MIPI DSI · CSI · ISP · H.264 | — | — | — | — | — | ✅ | — | — |
| **Security** | | | | | | | | |
| AES · SHA · RSA | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| HMAC · Digital Signature | — | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| ECC | — | — | ✅ | ✅ | ✅ | ✅ | — | ✅ |
| Flash encryption | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| ESP-TEE | — | — | ✅ | ✅ | ✅ | — | — | — |
| **Low power** | | | | | | | | |
| LP / ULP core | ⏳ | — | ⏳ | ✅ | — | ⏳ | ⏳ | ⏳ |
<!-- chip-matrix:end -->

- ESP32-P4 has no radio. It gets Wi-Fi through [ESP-Hosted](HOSTED.md), with a second emulator that runs an ESP32-C5 or C6 as its co-processor.
- On a chip without an SDMMC host, you can connect an SD card to an SPI bus (`--sd FILE,cs=PIN`).
- esptool, GDB and the browser build work on every chip. [Virtual parts](PARTS.md) can be connected to any peripheral that the table marks as working.

## Chips

| Chip | CPU | Notes |
|------|-----|-------|
| ESP32 | Xtensa LX6, dual-core | Wi-Fi (2.4 GHz), Bluetooth LE with NimBLE, Ethernet, SD card, I2C, the SHA, AES and RSA accelerators, flash encryption, Secure Boot V2 and PSRAM work. LEDC, RMT, PCNT, I2S, MCPWM and the ADC are not emulated yet, and no device can be connected to its SPI master. |
| ESP32-C3 | RV32IMC, single-core | |
| ESP32-C5 | RV32IMAC, single-core | Wi-Fi works on 2.4 GHz. The 5 GHz band and 802.15.4 are not emulated yet. |
| ESP32-C6 | RV32IMAC, single-core, with an LP core | |
| ESP32-H2 | RV32IMAC, single-core | Bluetooth LE and 802.15.4. The chip has no Wi-Fi. |
| ESP32-P4 | RV32IMAFC, dual-core | The chip has no radio. It gets Wi-Fi through [ESP-Hosted](HOSTED.md). |
| ESP32-S3 | Xtensa LX7, dual-core | PSRAM works in quad and octal mode (`CONFIG_SPIRAM=y`). |
| ESP32-S31 | RV32IMAFC, dual-core | |

Every chip starts the way a real chip does: it runs its mask ROM, then the ESP-IDF second-stage bootloader, then your application. The ROMs are built into the `esp-emu` binary. A few ROM functions (printf, UART output, delays, Wi-Fi transmit) are handled directly by the emulator instead of being run instruction by instruction.

## CPU and memory protection

- **RISC-V**: RV32IMC on C3, RV32IMAC on C5, C6 and H2, and RV32IMAFC (with single-precision floating point) on P4 and S31.
- **Xtensa**: LX7 on S3 and LX6 on ESP32. Both are dual-core and include windowed registers, zero-overhead loops, MAC16 and the floating-point unit.
- **SIMD instructions**: P4 and S31 run the full PIE instruction set, which ESP-SR, esp-dl and esp-nn use. S3 runs its `ee.*` instructions. The results of both were compared with real chips and match bit for bit.
- **Memory protection (PMP and PMA)**: the emulator enforces RISC-V physical memory protection and Espressif's PMA. Firmware gets the same access faults as on a real chip, for example in ESP-IDF's panic and memory-protection tests.
- **Access permission control**: the emulator enforces the APM and TEE permission controllers (C5, C6, H2, S31), the PMS on P4, and the PMS monitors on C3 and S3, for both the CPU and DMA. ESP-TEE and other firmware that isolates code see the same faults and violation reports as on a real chip.

## Wireless and networking

- **Wi-Fi**: the firmware connects to an access point that is built into the emulator. The access point supports WPA2-PSK, WPA3-SAE (with PMF, H2E and transition mode) and WPA2/WPA3-Enterprise (it forwards 802.1X to a RADIUS server). It has a DHCP server and connects the firmware to your network. Wi-Fi CSI works through the standard `esp_wifi_set_csi` API on C3, C5, C6 and S3, with generated data or a recording (`--csi-replay`). See [NETWORKING.md](NETWORKING.md).
- **Bluetooth LE**: the NimBLE host stack works on every chip with Bluetooth, and Bluedroid works on ESP32-S31. Rust firmware built with [esp-hal](https://github.com/esp-rs/esp-hal) and `esp-radio` works on ESP32-C3, C5, C6 and H2. The firmware talks to a [virtual controller](BLE.md#virtual-controllers) or to a [real Bluetooth adapter on Linux](BLE.md#physical-adapter-linux). See [BLE.md](BLE.md).
- **Thread / 802.15.4**: the OpenThread `ot_cli` and `ot_br` examples run on ESP32-C6 and ESP32-H2. One emulator forms a Thread network on its own, and two emulators on the same computer join one Thread network. See [THREAD.md](THREAD.md).
- **Ethernet**: the built-in Ethernet MAC works on ESP32, ESP32-P4 and ESP32-S31. Firmware built for QEMU with `CONFIG_ETH_USE_OPENETH` also works.
- **Connection to your network**: user-mode networking needs no setup and works on any operating system. It includes NAT, DHCP, DNS, mDNS, IPv6, port forwarding (`hostfwd`) and ping. On Linux you can use a TAP interface instead, and on macOS vmnet. `net=CIDR` selects the subnet. See [NETWORKING.md](NETWORKING.md).
- **ESP-Hosted**: an ESP32-P4 host and an ESP32-C5 or C6 co-processor run in two emulators that are connected over SDIO. See [HOSTED.md](HOSTED.md).
- **Matter and RainMaker**: see [MATTER.md](MATTER.md) and [RAINMAKER.md](RAINMAKER.md).

## Peripherals

- **UART**: all ports work. UART0 is the console. UART1 and higher send data at the baud rate the firmware sets, and support software and hardware flow control and automatic baud rate detection. `--uart-tcp` and `--uart1-tcp` connect a port to a TCP socket.
- **USB Serial/JTAG**: works on every chip that has it, both as a console and as a port for flashing.
- **USB-OTG** on S3 (full speed), P4 (high speed and full speed) and S31 (high speed): the chip works as a USB device, with buffer DMA or in slave (FIFO) mode. A virtual USB host inside the emulator detects the device. A TinyUSB CDC-ACM port can be the console, with `esp_tinyusb` or in an Arduino sketch. For mass-storage and HID devices, a program connected with `--cosim-tcp` can act as the host (`tools/cosim-tcp.py PORT usb-host`).
- **SD card**: `--sd FILE` puts a card image in the SDMMC host of ESP32, P4, S3 or S31, and the standard `esp_vfs_fat_sdmmc_mount` mounts it. `--sd FILE,cs=PIN` connects the card to an SPI bus instead, for `esp_vfs_fat_sdspi_mount`. This does not work on ESP32 yet. Without a card, commands time out, as they do with an empty slot.
- **Basic blocks**: GPIO and IO_MUX, the system timer, the timer groups and their watchdogs, the RTC watchdog, the interrupt controllers, eFuse, SPI flash and GDMA.
- **SPI**: the chip can be an SPI master. It can also be an SPI slave of a master connected with `--cosim-tcp`. The standard `spi_master` and `spi_slave` drivers work.
- **I2C**: the chip is an I2C master. An EEPROM at address 0x50 is always present on the bus, and `--i2c-dev` adds simple register-based devices.
- **I2S**: standard, TDM and PDM modes work at the sample rate the firmware sets. Transmit can be looped back to receive, and audio can be recorded to or played from WAV files (`--i2s-wav`, `--i2s-wav-in`).
- **RMT, LEDC, PCNT and MCPWM**: these work, and their output signals have the correct timing.
- **ADC**: one-shot and continuous modes work on C3, C5, C6, H2, P4, S3 and S31. A reading gives the voltage that a virtual part applies to the pin. If no part is connected, it gives the level of the pin's pull-up or pull-down resistor. Calibrated readings return the applied voltage.
- **Event Task Matrix** on C5, C6, H2, P4 and S31: these events work: GPTimer alarms, MCPWM timer and comparator events, DMA end-of-frame events and GPIO edges. These tasks work: GPIO set, clear and toggle, and the GPTimer tasks. Other events and tasks are not emulated yet.
- **Camera (DVP)** on S3, P4 and S31: the standard `esp_driver_cam` driver receives frames from a virtual camera part, from a program connected with `--cosim-tcp`, or from files (`--camera-frames`).
- **LCD (i80 and RGB)** on S3, P4 and S31: `esp_lcd` works, and transfers take the time that the pixel clock set by the firmware requires.
- **MIPI DSI** on P4: `esp_lcd` starts a DPI panel and refreshes it. A program connected with `--cosim-tcp` receives the panel commands and the frames, and `tools/cosim-tcp.py PORT dsi-view` displays them.
- **MIPI CSI and the ISP** on P4: the CSI controller of `esp_driver_cam` receives a test pattern, or frames from a virtual camera part or a program connected with `--cosim-tcp`. The frames pass through the ISP. `--csi-sensor` answers the driver when it looks for a camera sensor.
- **H.264 encoder** on P4: it encodes video, and `--h264-dump` writes the stream to a file.
- **JPEG codec and PPA** on P4 and S31: the standard `esp_driver_jpeg` driver encodes and decodes pictures, and `esp_driver_ppa` scales, rotates, blends and fills them.

## Security and crypto

- **Crypto accelerators**: AES (ECB, CBC, OFB, CTR, CFB), SHA (1, 224, 256), RSA, ECC, HMAC-SHA256 and the Digital Signature unit work on the chips that have them.
- **ECDSA**: P-256 and P-384 on P4 and C5, and P-256 on H2.
- **Key Manager**: works on P4 and C5, with the HUK generator. It can provide the keys for flash encryption, HMAC, Digital Signature and ECDSA.
- **Flash encryption**: works on every chip. `tools/make-encrypted-flash.py` creates an image that is already encrypted, so that the chip does not need to encrypt it on the first boot. See [CLI.md](CLI.md#flash-encryption-without-the-first-boot).
- **ESP-TEE**: works on C5, C6 and H2.

## Low power

- **LP core on ESP32-C6**: ESP-IDF `ulp_lp_core` firmware runs on the low-power core, next to the main core. The main core or the LP timer can start it. Both cores share the LP memory, and `lp_core_printf` prints on the console.
- **Sleep on ESP32-C6**: deep sleep and light sleep work. The RTC timer and the LP core can wake the chip. The LP core keeps running while the main core sleeps. LP IO, LP I2C, and wake-up from GPIO or UART are not emulated yet.
- **Sleep on ESP32-C3**: deep sleep and light sleep work. The RTC timer and GPIO levels can wake the chip (`esp_deep_sleep_enable_gpio_wakeup`, `gpio_wakeup_enable`).

## Tools and debugging

- **Flashing tools**: `esptool`, `espefuse` and `idf.py flash` work with the running emulator over `socket://`. Start the emulator with `--uart-tcp` and `--strap-mode 0x02`. See [FLASHING.md](FLASHING.md).
- **GDB**: `--gdb PORT` starts a GDB server, like `-s -S` in QEMU. Connect with `riscv32-esp-elf-gdb` or the Xtensa GDB for your chip.
- **Tracing**: `--trace FILE` records which task runs when, in a file that Perfetto opens. The firmware needs no changes. See [HOST-INTERFACES.md](HOST-INTERFACES.md#execution-tracing-and-profiling).
- **Control while running**: with `--control-tcp`, a script can reset the chip, hold its EN pin low, and read or write the flash while the emulator runs. See [HOST-INTERFACES.md](HOST-INTERFACES.md).

## Virtual parts

A virtual part is a component connected to the emulated chip, such as a button or a sensor. The firmware uses it with the standard drivers, as it would use the real component. The examples include a push button, an SHT31 sensor on I2C, a TMP123 sensor on SPI, an MH-Z19B CO₂ sensor on a UART, an SG90 servo on a PWM pin, a potentiometer on an ADC pin, a microphone and an amplifier on I2S, an OLED screen, a DHT22 sensor, a WS2812 LED strip, an IR receiver and camera sensors. Parts work in the command-line emulator and in the browser build.

[PARTS.md](PARTS.md) shows how to run the examples and how to write your own part. The example parts are in [`examples/parts/`](../../examples/parts), and the boards that connect them to a chip are in [`examples/boards/`](../../examples/boards). [`examples/parts/pushbutton`](../../examples/parts/pushbutton) is a complete part that you can copy as a starting point.

## Browser build

The emulator is also built as WebAssembly. It runs in a browser tab and has a JavaScript API. You can try it at <https://espressif.github.io/esp-emulator/>. See [BROWSER.md](BROWSER.md).
