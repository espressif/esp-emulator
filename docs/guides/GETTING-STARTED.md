# Getting Started

This guide shows how to install esp-emu, build a firmware image and run it. [FEATURES.md](FEATURES.md) lists what each chip supports, and [CLI.md](CLI.md) describes every command-line flag.

## Install

On Linux (x86_64, arm64) and macOS (Apple Silicon):

```sh
curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh | sh
```

This downloads the latest version to `$HOME/.local/bin/esp-emu`. If `~/.local/bin` is not in your `PATH`, the installer prints the `export PATH=...` line that you need to add.

To install a specific version:

```sh
curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh | sh -s -- --version 1.3.0
```

The installer has more options: `--check` (print the latest version and install nothing), `--bin-dir DIR`, `--force` and `--quiet`. `--help` lists all of them:

```sh
curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh | sh -s -- --help
```

### Update

```sh
esp-emu update           # update in place
esp-emu update --check   # only print the latest version
esp-emu --version        # print the installed version
```

`esp-emu update` runs the installer again for the directory that contains the `esp-emu` you started, so it updates that copy.

The emulator can also update itself in the background. This is off by default. To turn it on, run `esp-emu config autoupdate on`. To turn it off, run `esp-emu config autoupdate off` or set `ESP_EMU_DISABLE_AUTOUPDATES=1`. When it is on, esp-emu checks for a newer release each time it starts and installs it while your firmware runs. You get the new version the next time you start esp-emu.

### Install by hand

```sh
# Download the archive for your platform from
# https://github.com/espressif/esp-emulator/releases/latest
tar -xzf esp-emu-<version>-<platform>.tar.gz
cp esp-emu-<version>-<platform>/esp-emu ~/.local/bin/
~/.local/bin/esp-emu --help
```

### What a release contains

Each [GitHub release](https://github.com/espressif/esp-emulator/releases) contains the files below. Its release notes describe what changed.

| File | Contents |
|------|----------|
| `esp-emu-<version>-x86_64-unknown-linux-gnu.tar.gz` | Linux x86_64 |
| `esp-emu-<version>-aarch64-unknown-linux-gnu.tar.gz` | Linux arm64 |
| `esp-emu-<version>-aarch64-apple-darwin.tar.gz` | macOS Apple Silicon |
| `esp-emu-<version>-wasm.tar.gz` | the browser build ([BROWSER.md](BROWSER.md)) |
| `esp-emu-<version>-examples.tar.gz` | the example boards, each with its parts and firmware ready to run ([PARTS.md](PARTS.md)) |
| `SHA256SUMS` | checksums, which `install.sh` verifies |

Each of the first three archives contains a single file, the `esp-emu` executable. The ROMs of all chips are built into it.

`esp-emu-relay`, the network relay for the browser build, has its own releases, named `relay-v<version>`. They are available for Linux (x86_64, arm64), macOS (Intel, Apple Silicon) and Windows (x86_64). A relay works with any browser build that has the same major version. Its source code is in [`esp-emu-relay/`](../../esp-emu-relay).

## Run firmware

### Build a firmware image

The emulator runs the merged flash image of an ESP-IDF project. This is the same data that you would flash to a board, in one file. In an ESP-IDF environment (`source export.sh`):

```sh
cd your-esp-idf-project
idf.py set-target esp32c3    # or esp32, esp32c5, esp32c6, esp32h2, esp32p4, esp32s3, esp32s31
idf.py build
idf.py merge-bin             # writes build/merged-binary.bin
```

### Run it

```sh
esp-emu --chip esp32c3 --firmware build/merged-binary.bin
```

The firmware's console output appears in your terminal, and what you type is sent to the firmware's console. Press Ctrl-C to stop the emulator.

The mask ROM of each chip is built into esp-emu (ESP32 rev 3, C3 rev 3, C5 rev 1.0, C6 rev 0, H2 rev 0, P4 rev 3, S3 rev 0, S31). You need `--rom <path>` only if you want another chip revision or your own ROM, for example one from `~/.espressif/tools/esp-rom-elfs/`.

### Common options

| Flag | Default | Description |
|------|---------|-------------|
| `--chip <CHIP>` | (required) | `esp32`, `esp32c3`, `esp32c5`, `esp32c6`, `esp32h2`, `esp32p4`, `esp32s3` or `esp32s31` |
| `--firmware <PATH>` | (required) | The merged flash image |
| `--elf <PATH>...` | — | The application ELF file. It gives function names in backtraces and is required for Bluetooth LE and Thread |
| `--timeout <DURATION>` | — | Stop after this long (`5s`, `500ms`) |
| `--exit-on <STRING>` | — | Stop with exit code 0 when the console prints this text |
| `--inject <DATA>` | — | Text to send to the console (`\n` for a newline). Can be repeated |
| `--inject-on <STRING>` | — | Console output that triggers the matching `--inject`. Can be repeated |
| `--net <BACKEND>` | `tap0` on Linux if it exists, otherwise `user` | `user` (no setup, any OS), `user,hostfwd=tcp::H-:G`, `tap,ifname=tap0` (Linux), `vmnet` (macOS) |
| `--wifi-ssid <SSID>` | `myssid` | The name of the Wi-Fi network the firmware finds |
| `--wifi-password <PASS>` | `mypassword` | Its password; an empty string for an open network |
| `--ble-hci <BACKEND>` | — | `tcp:host:port` for a [virtual controller](BLE.md#virtual-controllers), `hci0` for a [Linux adapter](BLE.md#physical-adapter-linux) |
| `--uart-tcp <HOST:PORT>` | — | Connect UART0 to a TCP socket, so that esptool can use `socket://` |
| `--gdb <PORT>` | — | Start a GDB server on this port; `--gdb-halt` waits for the debugger to connect |
| `--trace <FILE>` | — | Record a per-task timeline for Perfetto; needs `--elf` |

[CLI.md](CLI.md) describes every flag, and how to set the flash size, the PSRAM size, I2C devices and the chip revision.

## Helper tools

The [`tools/`](../../tools) folder contains scripts that work with the emulator:

| Tool | What it does |
|------|--------------|
| `setup-tap.sh` | creates the `tap0` device for TAP networking on Linux |
| `cosim-tcp.py PORT DEVICE` | device models over `--cosim-tcp`: a button, a display viewer, an I2C board, I2S audio, a camera, an SPI master, a voltage source on an ADC pad, a USB host, a clock holder. Run it with no arguments to list them |
| `renode/` | a bridge that makes the chip an SPI slave of a machine in Renode |
| `bumble_test.py` | a virtual BLE controller (Google Bumble) over TCP |
| `bumble_classic_peer.py` | a virtual controller and a Classic Bluetooth peer for ESP32-S31 Bluedroid examples |
| `vhci_bridge.py` | a bridge to a Linux VHCI Bluetooth device |
| `ws-net-proxy.py` | a WebSocket-to-TAP bridge for your own web page that uses the browser build |
| `make-efuse.py` | writes an `--efuse` file that reports a given chip revision |
| `make-encrypted-flash.py` | encrypts a flash-encryption build on your computer and writes the eFuse file that goes with it |
| `trace-view.py` | shows CPU time per task and the hottest functions of a `--trace` file |

## Test apps

The [`test_apps/`](../../test_apps) folder contains the ESP-IDF projects that are used to test the emulator. `test_apps/run_smoke.sh` builds the main test app for every chip and runs it in the `esp-emu` that is in your `PATH`. It builds with the `espressif/idf:latest` Docker image, or with your own ESP-IDF if you pass `--idf-path`. The test passes when the app's `RESULT_SUMMARY:` line reports no failures.

```sh
test_apps/run_smoke.sh --chips esp32c3,esp32c6
```
