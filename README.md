# Emulator for ESP32 Series SoCs (Beta)

A Rust-based emulator that runs ESP32-C3, ESP32-C5, ESP32-C6, ESP32-H2, ESP32-P4, ESP32-S31 (RISC-V) and ESP32-S3 (Xtensa LX7) firmware binaries — CPU, memory, WiFi, BLE, Thread, Ethernet, crypto, and more. This repo distributes the installer, prebuilt release binaries, user-facing docs, and helper tools.

## Install

One-liner (Linux x86_64 / arm64, macOS Apple Silicon):

```sh
curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh | sh
```

This downloads the latest binary to `$HOME/.local/bin/esp-emu`. If `~/.local/bin` is not on your `PATH`, the installer prints the `export PATH=...` line to add.

Pin a specific version:

```sh
curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh | sh -s -- --version 0.45.0
```

Other options: `--check` (print latest, no install), `--bin-dir DIR`, `--force`, `--quiet`. Full help:

```sh
curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh | sh -s -- --help
```

## Updating

After install:

```sh
esp-emu update           # refresh in place
esp-emu update --check   # only print latest version
esp-emu --version        # print currently installed version
```

`esp-emu update` re-runs the installer against the directory holding the running binary, so it updates wherever you first installed it.

## Features

- **CPU**: Full RV32IMAC on C3/C5/C6/H2; RV32IMAFC (with single-precision FP via Berkeley SoftFloat) on P4; Xtensa LX7 (dual-core, windowed registers, zero-overhead loops, FPU) on S3. Multi-hart scheduler supports P4's dual HP cores. RV32 PMP and Espressif PMA enforced via a fused two-level page table — catches the same access violations as silicon (IDF panic memprot tests, TEE REE-vs-TEE isolation, IRAM/IROM write protection). P4 also runs the PIE SIMD instructions that ESP-SR, esp-dl and esp-nn use.
- **Access permission control**: the APM and TEE permission controllers (C5, C6, H2, S31) and P4's PMS are enforced for the CPU and DMA, so ESP-TEE and other isolation firmware sees the same faults and violation reports as silicon
- **WiFi**: Soft AP with WPA2-PSK, WPA3-SAE (PMF, H2E, transition mode) and WPA2/WPA3-Enterprise (802.1X relayed to a RADIUS server), 802.11 management frames, DHCP server, and TAP networking for real connectivity
- **Ethernet**: OpenCores Ethernet MAC (OpenETH) for QEMU-compatible `CONFIG_ETH_USE_OPENETH` firmware, plus Synopsys DesignWare GMAC for ESP32-P4's built-in EMAC
- **Networking backends**: user-mode (zero-setup, QEMU-style NAT via smoltcp — DHCP, DNS forwarder, mDNS relay with optional record-rewriting NAT for Matter/HomeKit-style service discovery, IPv6 SLAAC, `hostfwd`, restrict mode, ICMP echo), TAP bridge (Linux), vmnet (macOS)
- **BLE**: NimBLE host stack support with HCI forwarding to Bumble (virtual controller) or physical Linux HCI adapters
- **Thread / 802.15.4**: OpenThread `ot_cli` and `ot_br` on ESP32-C6 and ESP32-H2. Single-node forms a partition out of the box; two emulator instances form one Thread mesh (Leader + Child, or Border Router + end device) over a localhost UDP bridge
- **Crypto**: AES (ECB/CBC/OFB/CTR/CFB), SHA (1/224/256), RSA, ECC, HMAC-SHA256, Digital Signature, XTS-AES flash encryption, ECDSA (P-256/P-384 on P4 and C5; P-256 on H2), Key Manager + HUK Generator (P4, C5) — drives flash / HMAC / DS / ECDSA key sourcing for `CONFIG_SECURE_FLASH_ENCRYPTION_KEY_SOURCE_KEY_MGR`
- **Peripherals**: UART, USB Serial JTAG, GPIO, system timer, timer groups, interrupt controllers (PLIC for C3/C6/H2, CLIC for C5/P4), eFuse, SPI flash, GDMA, GP-SPI, RMT, LEDC, PCNT, MCPWM, I2C (with a built-in EEPROM slave), and the TIMG/RTC watchdogs
- **esptool / espefuse over `socket://`**: a `--uart-tcp HOST:PORT` bridge plus `--strap-mode 0x02` (UART download) lets esptool, espefuse, and `idf.py flash` drive the running emulator over TCP, matching the QEMU-Espressif workflow. See [esptool / espefuse](#esptool--espefuse-over-socket).
- **Chips**: ESP32-C3, ESP32-C5, ESP32-C6, ESP32-H2, ESP32-P4, and ESP32-S3 (dual-core Xtensa; boots the real mask ROM through the IDF bootloader to app_main; 2.4 GHz WiFi station and BLE both work against the built-in controllers; external PSRAM works in both quad and octal mode, `CONFIG_SPIRAM=y`) with per-chip memory maps and interrupt controllers. C5 is a hybrid: a C6-shaped peripheral map driven by a P4-style CLIC on a single core (2.4 GHz WiFi and BLE both work; 5 GHz and 802.15.4 are not modelled). (ESP32-S31 is in early bring-up)
- **Debugging**: `--gdb PORT` serves a GDB remote stub (QEMU `-s -S` equivalent) for `riscv32-esp-elf-gdb` / `xtensa-esp32s3-elf-gdb`; `--trace FILE` records a per-task timeline for Perfetto with no guest instrumentation. See [Execution Tracing and Profiling](#execution-tracing-and-profiling).
- **WASM**: Browser-based emulation via WebAssembly with JavaScript API
- **ROM stubs**: Intercepts key ROM functions (printf, UART, delay, WiFi TX) instead of emulating full ROM

## Quick Start

### Prerequisites

- A merged flash binary built with ESP-IDF (see [Building Firmware](#building-firmware-images))

Default ROM ELFs (C3 rev3, C5 rev100, C6 rev0, H2 rev0, P4 rev3, S3 rev0) are embedded in the binary, so `--rom` is optional for the common case. Pass `--rom <path>` only to override with a different silicon revision or a custom ROM (e.g. from `~/.espressif/tools/esp-rom-elfs/`).

### Running (quick ESP-IDF example)

```sh
cd esp-idf
cd examples/protocols/https_request
idf.py set-target esp32c3
idf.py merge-bin
esp-emu --chip esp32c3 --firmware build/merged-binary.bin
```

### CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `--chip <CHIP>` | (required) | Target chip: `esp32c3`, `esp32c5`, `esp32c6`, `esp32h2`, `esp32p4`, `esp32s3`, or `esp32s31` |
| `--firmware <PATH>` | (required) | Path to merged flash binary |
| `--rom <PATH>` | embedded | Path to ROM ELF file (overrides the built-in default for `--chip`) |
| `--elf <PATH>` | — | Path to application ELF for BLE symbol lookup (e.g. `build/project.elf`) |
| `--efuse <PATH>` | — | Path to eFuse binary (336 bytes, QEMU-compatible). State is always saved back on exit (eFuses are one-time-programmable). |
| `--timeout <DURATION>` | — | Exit after duration (e.g. `5s`, `500ms`) |
| `--exit-on <STRING>` | — | Exit with code 0 when UART output contains this string |
| `--batch-size <N>` | `50000` | Instructions per iteration |
| `--skip-bootloader` | off | Load app directly from partition table, skipping 2nd-stage bootloader (firmware-entry path; only meaningful with `--skip-rom`) |
| `--skip-rom` | off | Reset the CPU at the firmware/bootloader entry instead of the real mask ROM. Default executes the ROM, which is required for KEY_SOURCE_KEY_MGR flash-key recovery and other eFuse-driven cold-boot decisions. |
| `--save-state` | off | Save flash state to disk on exit (overwrites firmware file) |
| `--inject <DATA>` | — | Payload to inject into UART RX (supports `\n` escape). Repeatable. |
| `--inject-on <STRING>` | — | Trigger string in UART output that causes injection (paired with `--inject`). Repeatable. |
| `--net <BACKEND>` | `tap0` if present on Linux, else `user` | Network backend: `user` (slirp-style, any OS), `user,hostfwd=tcp::H-:G,…`, `user,restrict=yes`, `user,dns=1.1.1.1`, `user,mdns-nat=yes` (Matter / mDNS service NAT), `tap,ifname=tap0` (Linux), `vmnet` (macOS) |
| `--wifi-ssid <SSID>` | `myssid` | WiFi soft AP SSID broadcast to firmware |
| `--wifi-password <PASS>` | `mypassword` | Passphrase (8-63 chars) for WPA2-PSK and WPA3-SAE. Empty string for open mode |
| `--wifi-auth <MODE>` | `wpa2-wpa3` | Soft AP security: `wpa2-wpa3` (transition), `wpa3-sae` (WPA3 only, PMF required), `wpa2-psk`, `open`, `wpa2-enterprise`, `wpa3-enterprise` |
| `--wifi-radius <HOST:PORT,SECRET>` | — | RADIUS server the enterprise modes relay EAP to |
| `--wifi-sae-pwe <PWE>` | `both` | SAE password element the AP accepts: `both`, `h2e`, `hunt-and-peck` |
| `--ble-hci <BACKEND>` | — | BLE HCI backend: `tcp:host:port` for Bumble/virtual controller, `hci0` for Linux adapter |
| `--thread-sim <SPEC>` | — | IEEE 802.15.4 / Thread bridge, e.g. `bind:9001,peer:127.0.0.1:9002`. Forwards radio frames over localhost UDP to another emulator instance. Requires `--elf`. |
| `--uart-tcp <HOST:PORT>` | — | Bridge UART0 to a TCP server (e.g. `127.0.0.1:5555`); esptool connects via `socket://`. Mirrors QEMU's `-serial tcp::PORT,server,nowait`. While active, UART RX comes from the socket and TX goes to it (stdin/stdout disconnected). |
| `--uart1-tcp <HOST:PORT>` | — | Bridge UART1 to a TCP server. Side channel for simulating an external serial device (sensor, GPS, modem) with a host-side script — UART0 keeps stdin/stdout and `--exit-on`/`--inject-on`. Without a connected client, UART1 TX is discarded. |
| `--psram-size <SIZE>` | `8M` (C5, S3), `16M` (P4, S31) | External RAM the board is modelled with: `4M`/`8M`/`16M`/`32M`/`64M`, or `0` for no device. Only the chips with external RAM take it. Firmware cannot choose — see [PSRAM Size](#psram-size). |
| `--strap-mode <HEX>` | — | GPIO_STRAP value at reset. `0x02` = UART download mode (jumps to ROM entry instead of firmware entry; remapped per chip, so pass `0x02` on every target); `0x04` = USB-Serial-JTAG download; `0x08` = SPI flash boot (default). Mirrors QEMU's `-global driver=esp32cN.gpio,property=strap_mode,value=…`. |
| `--control-tcp <HOST:PORT>` | — | Host control channel: newline-delimited `reset`, `reset --soft`, `erase-flash`, `erase-region OFF LEN`, `write-region OFF FILE`, `ping`. Acts on the running machine, so one process and one output stream survive a reset — see [Host Control Channel](#host-control-channel). |
| `--gdb <PORT>` | — | Serve a GDB remote stub on this port (QEMU's `-s` is 1234). `riscv32-esp-elf-gdb` / `xtensa-esp32s3-elf-gdb` connect with `target remote :PORT`. |
| `--gdb-halt` | off | Hold the CPU at the reset vector until a GDB client connects (QEMU's `-S`). |
| `--trace <FILE>` | — | Write a per-task Chrome Trace Event trace for Perfetto; requires `--elf`. `--trace-pc-period N` sets the PC sample rate for the hot-symbol table (default 16 ticks, 0 disables). See [Execution Tracing and Profiling](#execution-tracing-and-profiling). |
| `--rmt-loopback <TX:RX>` | — | Feed an RMT TX channel's symbols into an RX channel, standing in for a wire between two pins (runs the stock `ir_nec_transceiver` example). |

### UART Injection

The `--inject` and `--inject-on` flags work in pairs to send data to the firmware's UART when specific output is detected:

```sh
esp-emu \
  --firmware app.bin \
  --inject "yes\n" --inject-on "Continue? [y/n]"
```

Standard input is also forwarded to UART RX line-by-line.

### UART1 TCP Bridge

`--uart1-tcp HOST:PORT` exposes UART1 as a TCP server, independent of the
UART0 console. Use it to simulate an external serial device (GPS module,
sensor, modem) with a host-side script:

```sh
esp-emu --chip esp32c3 \
  --firmware build/merged_flash.bin \
  --uart1-tcp 127.0.0.1:5556
```

```py
# Host-side mock device: talk to the firmware's UART1
import socket
s = socket.create_connection(("127.0.0.1", 5556))
s.sendall(b"$GPGGA,123519,4807.038,N\n")   # firmware sees it on UART1 RX
print(s.recv(4096))                         # firmware's UART1 TX
```

Works on all supported chips (verified with ESP-IDF's `uart_echo` example on
C3 and P4). One client at a time; the listener stays up across reconnects.
When no client is connected, firmware TX on UART1 is discarded — same as
QEMU's unattached chardev.

### Host Control Channel

`--control-tcp HOST:PORT` serves the device-state operations a UART socket
cannot express. On hardware a reset is a DTR/RTS toggle into EN; pyserial's
`socket://` transport drops modem-control lines, so `dut.serial.hard_reset()`
and friends have no in-band form. The channel fills the role QEMU gives QMP,
without the protocol: newline-delimited commands, one reply per line (`ok`, a
value, or `err: ...`).

| command | effect |
|---|---|
| `reset` | chip reset (what a DTR/RTS toggle does on a board) |
| `reset --soft` | software reset |
| `erase-flash` | as `esptool erase-flash`, on the running machine's flash |
| `erase-region OFFSET LENGTH` | as `esptool erase-region`; numbers in hex or decimal |
| `write-region OFFSET FILE` | as `esptool write-flash`, reading the data from a path |
| `ping` | answers `ok` |

```sh
esp-emu --chip esp32c3 --firmware build/merged_flash.bin \
  --uart-tcp 127.0.0.1:5555 --control-tcp 127.0.0.1:5556

printf 'erase-region 0x9000 0x6000\nreset\n' | nc 127.0.0.1 5556   # wipe NVS, reboot
```

The emulator process keeps running across a reset, so a harness such as
pytest-embedded keeps one continuous output stream, expect history and log.
Flash operations act on the flash the running firmware sees; rewriting the image
file from outside would not, since the flash contents are carried across resets
in memory.

### Log Levels

Control verbosity via `RUST_LOG`:

```sh
RUST_LOG=info  esp-emu ...   # Default, boot messages
RUST_LOG=debug esp-emu ...   # Peripheral access details
RUST_LOG=trace esp-emu ...   # Every bus read/write
```

### Flash Size

The emulated flash device is sized from the **image header**, not the length of
the file you pass to `--firmware` (a merged image is padded to wherever the last
segment lands, which says nothing about the part on the board). The size lives
in the upper nibble of byte 3 of the header — at flash offset `0x3` on C3/C6/H2
and `0x2003` on C5/P4/S31 — and is whatever `CONFIG_ESPTOOLPY_FLASHSIZE` you
built with. Every size esptool defines is honoured, 1 MB through 128 MB.

`RUST_LOG=debug` prints the size that was picked (`Flash resized to N KB`).

Past 16 MB a 24-bit address can no longer name every byte, so ESP-IDF sets
`CONFIG_BOOTLOADER_FLASH_32BIT_ADDR` and the `spi_flash` driver switches to the
4-byte-address opcodes (`0x13` read, `0x12` page-program, `0x21` / `0xDC`
erase, and the dual/quad read variants). Those are modelled, so a data
partition above the 16 MB line reads and writes normally.

Two limits are the silicon's, and the emulator reproduces both:

- **C6 and H2 refuse flash over 16 MB.** ESP-IDF's own
  `esp_mspi_32bit_address_flash_feature_check()` returns `ESP_ERR_NOT_SUPPORTED`
  there ("32bit address (flash over 16MB) has high risk on this chip"), and the
  app-init assert aborts the boot. Use C3, C5, P4 or S31.
- **Cache-mapped access stays under 16 MB** on quad flash unless the
  experimental `CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH` is on. App and
  OTA partitions execute through the cache, so keep them below the line; data
  partitions (FAT, LittleFS, NVS, SPIFFS) reached through `esp_partition_read`
  / `esp_flash_read` go anywhere. ESP-IDF enforces this itself — without the
  option `spi_flash_mmap()` rejects a `src_addr` at or above 16 MB with
  `ESP_ERR_INVALID_ARG` and an error naming the config — so the emulator needs
  no check of its own. With the option on, mapping above the line works here
  too. The one thing not modelled is the cache's own read command: the MMU
  resolves an entry by reading the flash array rather than replaying the SPI
  read the cache would issue, so firmware that programs MMU registers directly
  (rather than going through `esp_mmu_map`) can map past 16 MB here in a
  configuration where hardware would return aliased data.

### PSRAM Size

The emulated PSRAM die reports **16 MB** by default on ESP32-P4 and ESP32-S31,
and 8 MB on ESP32-C5 and ESP32-S3. `--psram-size` changes it:

```sh
esp-emu --chip esp32p4 --firmware app.bin --psram-size 32M
```

Unlike flash, this is not something firmware can override. `esp_psram` has no
size option in Kconfig for P4/S31 — line mode and clock speed only — and takes
the boot-time mode-register probe as the answer: MR2's 3-bit density field,
decoded by `esp_psram_impl_ap_hex.c` / `_ap_oct.c` as 4/8/16/32/64 MB. C5's
quad device states its size in the `0x9F` device ID instead, and names 2 MB
through 32 MB. S3 is the one chip whose board picks the family — a quad die
(`CONFIG_SPIRAM_MODE_QUAD`, the `0x9F` ID) or an octal one (`_OCT`, MR2
density) — so it names 2 MB through 64 MB, and a size only one family can
express fits that die and reports the other absent, as a real board does. So
the size is the board's, which makes it the emulator's to
state, and `--psram-size` is how you reproduce a 32 MB module's heap exhaustion
paths or framebuffer placement (GH #9).

The flag moves the die's answer and the backing store together, so what
firmware detects is what it can actually use. It does not move the address
window: that is the SoC's (64 MB on P4/S31), and a read past the device returns
0 the way an absent chip does.

`--psram-size 0` fits no die at all. The driver's connectivity check then fails
and firmware takes its not-found path — `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` boots
anyway, without it `esp_psram` aborts, which is exactly what the hardware does
with an unpopulated footprint.

A size no device ID can name is refused at startup rather than answered wrong:

```
$ esp-emu --chip esp32p4 --firmware app.bin --psram-size 12M
Error: esp32p4 cannot report a 12582912-byte PSRAM; its device ID names 4M, 8M, 16M, 32M, 64M (or 0 for none)
```

### Chip Revision (eFuse)

Firmware built with a `CONFIG_<CHIP>_REV_MIN_*` constraint checks the wafer
revision the eFuses report; with no `--efuse` file the emulator seeds each
chip's default (C3 v1.3, for example). To run a production binary that needs a
specific revision, build an eFuse blob stating it instead of forking the
sdkconfig:

```sh
# make-efuse.py ships in this repo's tools/ directory
python3 tools/make-efuse.py --chip esp32c3 --chip-rev 1.1 -o c3_rev1p1.efuse
esp-emu --chip esp32c3 --firmware build/merged_flash.bin --efuse c3_rev1p1.efuse
```

`--chip-rev` takes `MAJOR.MINOR` or ESP-IDF's integer form (`101`). The script
covers C3, C5, C6, H2, P4 and S31, writes the MAC and calibration defaults the
emulator would otherwise seed, and warns past each chip's `REV_MAX_FULL`, where
the bootloader rejects every image. espefuse over `socket://` cannot do this:
BLK1 is Reed-Solomon coded and the burn aborts once the seeded revision leaves
it non-empty. An `--efuse` image that states any wafer field is honoured as
written; the default is seeded only when all of them read zero.

## WiFi Emulation

The emulator includes a built-in WiFi soft access point with WPA2-PSK and WPA3-Personal (SAE) support. Firmware that connects to WiFi will:

1. **Scan** — The AP sends beacons and probe responses with the configured SSID
2. **Authenticate** — Open System, or SAE for WPA3
3. **Associate** — AP assigns AID=1
4. **4-way handshake** — Full EAPOL handshake with the PSK or SAE key (when password is set)
5. **DHCP** — Built-in DHCP server assigns 192.168.4.2 (gateway 192.168.4.1)

This works automatically — ESP-IDF WiFi station firmware will connect and receive an IP address. Use `--wifi-ssid` and `--wifi-password` to match your firmware's WiFi configuration. Default: SSID `myssid`, password `mypassword`.

With a password set, the AP runs WPA2/WPA3 transition mode, so there is nothing to configure per firmware: a WPA2 station uses WPA2-PSK, a WPA3 station uses SAE, and firmware that requires WPA3 with protected management frames connects as it would to a real WPA3 network. Reconnects reuse the cached SAE result.

**Enterprise (802.1X)**: `--wifi-auth wpa3-enterprise --wifi-radius 127.0.0.1:1812,testing123` makes the AP authenticate stations against a RADIUS server, so EAP-TLS, PEAP and TTLS work with whatever the server supports. hostapd's built-in EAP server is enough, using the certificates from ESP-IDF's `examples/wifi/wifi_enterprise`:

```
# hostapd.conf
driver=none
interface=lo
eap_server=1
radius_server_clients=clients          # "127.0.0.1 testing123"
radius_server_auth_port=1812
eap_user_file=eap_users                # "* PEAP,TTLS,TLS" and "\"espressif\" MSCHAPV2,TTLS-MSCHAPV2 \"test11\" [2]"
ca_cert=ca.pem
server_cert=server.crt
private_key=server.key
```

Run `hostapd hostapd.conf &`, then the example with `--wifi-ssid ESP_ENTERPRISE_AP` and the two flags above. `wpa2-enterprise` advertises 802.1X without PMF; `wpa3-enterprise` requires PMF. WPA3-Enterprise 192-bit (Suite-B) is not supported.

To test a firmware's security policy rather than its connection, pin the AP: `--wifi-auth wpa3-sae` is a WPA3-only network that refuses WPA2 stations, `--wifi-auth wpa2-psk` is a WPA2-only AP that WPA3-only firmware must reject, and `--wifi-sae-pwe hunt-and-peck` makes firmware that insists on hash-to-element fail as it would on an older AP.

For real network connectivity, run with either [User-mode Networking](#user-mode-networking-no-host-setup) (zero setup, the default when `--net` is omitted and no `tap0` is available) or [TAP Networking](#tap-networking) (bridged to host interface; picked automatically on Linux when `tap0` is set up).

## Ethernet Emulation (OpenETH)

The emulator includes an OpenCores Ethernet MAC, the same virtual NIC used by the Espressif QEMU fork. ESP-IDF firmware built with `CONFIG_ETH_USE_OPENETH=y` will use this driver for networking instead of WiFi.

OpenETH provides a simpler, faster networking path — raw Ethernet frames pass directly between the firmware and the TAP device without 802.11 frame wrapping or WPA2 encryption overhead.

```sh
# Build firmware with OpenETH (in your ESP-IDF project sdkconfig):
#   CONFIG_ETH_USE_OPENETH=y
#   CONFIG_EXAMPLE_CONNECT_ETHERNET=y

# Run with TAP networking (also requires dnsmasq for DHCP)
sudo dnsmasq --interface=tap0 --bind-interfaces --dhcp-range=192.168.4.2,192.168.4.100,12h
esp-emu \
  --chip esp32c6 \
  --firmware build/merged_flash.bin \
  --net "tap,ifname=tap0"
```

The routing is automatic: when firmware enables OpenETH (sets TXEN/RXEN in MODER), TAP frames go directly to the Ethernet MAC. When firmware uses WiFi instead, frames route through the WiFi soft AP as before. No CLI flag is needed to select the path.

## ESP32-P4 Support

ESP32-P4 is a dual-core RV32IMAFC chip with hardware single-precision floating point, a custom CLIC interrupt controller, and a Synopsys DesignWare GMAC at `0x50098000`. The emulator boots ROM → 2nd-stage bootloader → ESP-IDF app, runs SMP firmware (`xTaskCreatePinnedToCore` on both cores), and supports the built-in EMAC over any networking backend (`--net user`, TAP, vmnet).

```sh
esp-emu \
  --chip esp32p4 \
  --firmware build/merged_flash.bin \
  --net user
```

Build firmware with `idf.py set-target esp32p4 && idf.py build`. The emulator models ESP32-P4 v3.x silicon. For firmware built for an earlier revision, run it with an eFuse file stating that revision (see [Chip Revision (eFuse)](#chip-revision-efuse)).

CPU1 is brought up dynamically once the firmware releases its reset (`LP_AON_CLKRST_HPCPU_RESET_CTRL0`); single-core firmware (`CONFIG_FREERTOS_UNICORE=y`) runs on hart 0 only with no dual-core overhead. PSRAM is backed as zero-init RAM; PTP, jumbo Ethernet frames, and the LP core are not modelled.

## User-mode Networking (no host setup)

`--net user` enables a QEMU-style user-mode backend that proxies the guest's
TCP/UDP flows through host sockets. No TAP device, no `sudo`, no `dnsmasq` —
it works out of the box on Linux, macOS, and other Unix platforms.

```sh
esp-emu \
  --chip esp32c6 \
  --firmware build/merged_flash.bin \
  --net user
```

Addressing matches the WiFi soft AP (`192.168.4.0/24`, guest `192.168.4.2`,
gateway `192.168.4.1`). Traffic to the gateway IP is redirected to
`127.0.0.1` on the host, mirroring slirp's `10.0.2.2` convention.

### What's included

- **DHCP** + **ARP** so the guest can get an IPv4 lease on both WiFi and
  OpenETH paths
- **TCP/UDP outbound NAT** via smoltcp's TCP stack + non-blocking host sockets
- **Built-in DNS forwarder**: parses `/etc/resolv.conf`; the guest is told the
  gateway is its DNS server and queries are forwarded transparently
- **mDNS relay**: binds `224.0.0.251:5353` on the host (via `SO_REUSEPORT`, so
  it coexists with Avahi / systemd-resolved) and proxies multicast queries
- **IPv6**: smoltcp IPv6 stack, link-local + ULA addresses, Router Advertisement
  so the guest auto-configures a global IPv6 via SLAAC
  (`fd00:6573:702d:656d::/64`)
- **ICMP echo-reply spoof** — `ping` from the guest "succeeds" without real
  ICMP leaving the host (same as QEMU/libslirp without `CAP_NET_RAW`)

### Host → guest via `hostfwd` (QEMU-compatible syntax)

```sh
esp-emu ... --net "user,hostfwd=tcp::18080-:80,hostfwd=udp::1053-:53"

# From the host:
curl http://127.0.0.1:18080/hello
dig @127.0.0.1 -p 1053 example.com
```

Format: `hostfwd=PROTO:[HOST_ADDR]:HOST_PORT-:GUEST_PORT`. An empty host addr
(`tcp::10080-:80`) binds `0.0.0.0` (any interface) — match QEMU. Use
`tcp:127.0.0.1:10080-:80` to restrict to localhost.

### mDNS NAT (Matter, HomeKit, etc.)

Zero-setup Matter commissioning and control — no TAP needed. Adds on top of the
basic mDNS relay by **rewriting** the guest's A/AAAA records to host loopback
and automatically binding hostfwd listeners for each SRV-advertised port.

```sh
esp-emu ... --net "user,mdns-nat=yes"

# In another terminal:
chip-tool pairing ble-wifi 1 myssid mypassword 20202021 3840 --ble-controller 1
chip-tool onoff toggle 1 1
```

When the guest announces `_matter._tcp … port=5540 target=<name>.local` with
A/AAAA records pointing at `192.168.4.2` / `fd00:...`, the backend:

1. Rewrites A → `127.0.0.1`, AAAA → `::1` in the mDNS response (both the
   multicast announcements and the unicast replies to QU-bit queries).
2. Extracts the SRV port and auto-binds `127.0.0.1:5540` UDP + TCP hostfwds
   that forward to the guest.

chip-tool's Matter resolver sees the service at `127.0.0.1:5540`, the hostfwd
bridges the CASE/operational traffic into the guest, and post-commissioning
control (toggle, read, invoke, etc.) works without any kernel-level routing.
This is beyond what QEMU's libslirp does — QEMU silently drops all mDNS.

### Restrict mode

```sh
esp-emu ... --net "user,restrict=yes"
```

Blocks all guest-initiated TCP/UDP except DNS. Useful for CI to prove a
firmware doesn't phone home.

### Limitations vs. TAP

- **Not reachable from the host by IP**: `ping 192.168.4.2` does not work.
  The guest IP lives inside the emulator process; use `hostfwd` to expose
  specific ports instead.
- **No packet capture on a host interface**: `tcpdump` sees nothing because
  the frames never leave user space. Use `RUST_LOG=esp_emu::net_user=trace`
  for backend-level tracing.
- Not yet implemented: IP fragmentation, TFTP server, `guestfwd`.

## TAP Networking

TAP mode bridges the emulated network (WiFi or Ethernet) to a host network interface, giving the firmware real TCP/IP connectivity. WiFi firmware uses the built-in DHCP server; Ethernet (OpenETH) firmware needs an external DHCP server on the TAP interface (e.g., dnsmasq). Use this mode when you need real LAN visibility (`ping`, `tcpdump`, multi-emulator bridging) — otherwise `--net user` above is easier.

### Linux (TAP)

```sh
# Create TAP interface with NAT (auto-detects outbound interface)
sudo ./tools/setup-tap.sh

# Run with TAP
esp-emu \
  --chip esp32c3 \
  --firmware app.bin \
  --net "tap,ifname=tap0"

# Cleanup
sudo ./tools/setup-tap.sh --teardown
```

### macOS (vmnet)

```sh
# vmnet requires sudo or com.apple.vm.networking entitlement
sudo esp-emu \
  --chip esp32c3 \
  --firmware app.bin \
  --net vmnet
```

## BLE Emulation

BLE firmware (NimBLE host stack) runs natively in the emulator. HCI commands are either handled by a built-in virtual controller or forwarded to an external backend via `--ble-hci`. Requires `--elf` to provide firmware symbols for HCI interception.

### Bumble (software-only, no hardware needed)

[Google Bumble](https://github.com/google/bumble) acts as a virtual BLE controller and GATT client over TCP.

```sh
pip3 install bumble
python3 tools/bumble_test.py &        # Virtual controller on port 9544
esp-emu \
  --chip esp32c3 \
  --firmware build/merged_flash.bin \
  --elf build/bleprph.elf \
  --ble-hci tcp:localhost:9544
```

The test script scans, connects, discovers GATT services, reads/writes characteristics, and subscribes to notifications. Works with ESP32-C3, ESP32-C6, ESP32-H2, and ESP32-S3.

### Physical adapter (Linux)

Forward HCI to a real Bluetooth adapter for phone interaction:

```sh
sudo hciconfig hci0 down
sudo setcap 'cap_net_admin+eip' "$(command -v esp-emu)"
esp-emu \
  --chip esp32c3 \
  --firmware build/merged_flash.bin \
  --elf build/bleprph.elf \
  --ble-hci hci0
```

## Thread Emulation (ESP32-C6 / ESP32-H2)

OpenThread `ot_cli` runs on an unmodified ESP32-C6 or ESP32-H2 emulator
(substitute `esp32h2` for `esp32c6` below) — the node
boots, drives the CLI (`ot ifconfig up`, `ot thread start`, `ot state`,
`ot dataset …`), and becomes Leader of its own partition after MLE
attach times out (no peer to find).

```sh
esp-emu \
  --chip esp32c6 \
  --firmware /tmp/ot_cli_c6/build/merged_flash.bin \
  --elf /tmp/ot_cli_c6/build/esp_ot_cli.elf
```

For **two-node** Thread networks, `--thread-sim bind:P,peer:H:P` bridges
raw 802.15.4 frames between emulator instances over localhost UDP. With
both firmwares built using `OPENTHREAD_NETWORK_AUTO_START=y` (so they
share an identical compile-time Active Dataset), the second node joins
the first as a Child — verified with `ot state` + `ot parent` on the
device and matching `ot partitionid` on both sides.

```sh
# Border Router
esp-emu --chip esp32c6 \
  --firmware /tmp/ot_br_c6/build/merged_flash.bin \
  --elf /tmp/ot_br_c6/build/esp_ot_br.elf \
  --thread-sim "bind:9001,peer:127.0.0.1:9002" &
sleep 2
# End device
esp-emu --chip esp32c6 \
  --firmware /tmp/ot_cli_c6/build/merged_flash.bin \
  --elf /tmp/ot_cli_c6/build/esp_ot_cli.elf \
  --thread-sim "bind:9002,peer:127.0.0.1:9001"
```

See [docs/guides/THREAD.md](docs/guides/THREAD.md) for the full flow:
firmware builds (`ot_cli`, native-radio `ot_br`), shared-dataset setup,
trigger-based CLI injection, and troubleshooting.

## esptool / espefuse over `socket://`

esp-emu can expose its emulated UART as a TCP server so `esptool.py`, `espefuse.py`, and `idf.py flash` can drive it via `socket://host:port`, matching the workflow documented for [QEMU-Espressif](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/esp32c3/README.md#using-esptoolpy-and-espefusepy-to-interact-with-qemu).

### Setup

Two CLI flags work together:

- `--uart-tcp <HOST:PORT>` — bridges UART0 to a TCP server (mirrors QEMU's `-serial tcp::PORT,server,nowait`). One client at a time; the listener stays up across reconnects.
- `--strap-mode 0x02` — sets the strapping pin so the ROM enters UART download mode at reset. Without this the chip still boots normally from flash.

```sh
# Start the emulator in download mode with UART bridged to TCP 5555
esp-emu \
  --chip esp32c3 \
  --firmware build/merged_flash.bin \
  --efuse /tmp/qemu_efuse.bin \
  --strap-mode 0x02 \
  --uart-tcp 127.0.0.1:5555
```

In another shell, point esptool/espefuse at the socket. Both esptool's default
**stub-flasher mode** and `--no-stub` (ROM-loader) mode work; only `--before
no-reset --after no-reset` are required (see [Caveats](#caveats)):

```sh
# Identify the chip / read flash JEDEC (stub mode — esptool's default)
esptool.py -p socket://localhost:5555 --chip esp32c3 \
  --before no-reset --after no-reset flash_id

# Write part of flash
esptool.py -p socket://localhost:5555 --chip esp32c3 \
  --before no-reset --after no-reset \
  write_flash 0x100000 build/myapp.bin

# Burn an eFuse (custom MAC)
espefuse.py --port socket://localhost:5555 --chip esp32c3 \
  --do-not-confirm burn_custom_mac aa:bb:cc:dd:ee:ff

# Flash from idf.py
ESPPORT=socket://localhost:5555 idf.py flash
```

### What works

End-to-end verified on **C3, C5, C6, H2, P4, S3, and S31**:

| command                     | C3 | C5 | C6 | H2 | P4 | S3 | S31 |
|-----------------------------|----|----|----|----|----|----|-----|
| `esptool chip-id`           | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅¹ |
| `esptool flash-id`          | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `esptool read-mac`          | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `esptool read-flash`        | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `esptool write-flash`       | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `esptool verify-flash`      | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `esptool erase-region`      | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `espefuse summary`          | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `espefuse get-custom-mac`   | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `espefuse burn-custom-mac`  | ✅ | ❌² | ✅ | ✅ | ✅ | ✅ | ✅ |
¹ S31 has no chip ID; esptool reports the MAC instead (same as on silicon).

² `espefuse burn-custom-mac` is the one command C5 cannot run. espefuse derives
the crystal frequency from `UART_CLKDIV` and computes 26 MHz, which it rejects
because C5 accepts only 40 or 48 MHz. Only the burn path checks the crystal, so
`espefuse summary` and `get-custom-mac` work. Everything else on C5 passes —
stub and `--no-stub`, UART and USB-Serial/JTAG, flash write/read.

All commands work in both **stub mode** (esptool uploads its RAM flasher — the
default) and **`--no-stub` ROM mode**. On P4, stub-mode `write-flash` /
`verify-flash` / `erase-region` are verified byte-accurate (including
incompressible payloads). eFuse burns persist back to the file passed via
`--efuse <path>` on graceful exit (timeout, Ctrl+C, or `exit-on` match). Flash
writes persist with `--save-state`.

> **S31 strap note.** `--strap-mode 0x02` is the canonical "enter UART download"
> request on every chip, but the S31 mask ROM decodes strap bits differently:
> `0x02` is an *invalid* combination it `assert`s on, and the auto-detect
> `DOWNLOAD(USB/UART0/SPI)` arm resolves to USB-Serial-JTAG (so the flasher stub
> would answer over USB, not the TCP UART bridge). The emulator therefore remaps
> the canonical `0x02` to S31's `UART0_BOOT` strap (`0x04`) internally — you
> still pass `--strap-mode 0x02` exactly as on the other chips.

### Caveats

- **`--before no-reset --after no-reset` are required.** RTS isn't transmissible over a TCP socket, so esptool can't auto-reset the target. To reboot the emulated chip without restarting the process, send `reset` over `--control-tcp` (see [Host Control Channel](#host-control-channel)).
- **`--no-stub` is an optional fallback.** Stub mode is the default and works for everything above; `--no-stub` uses the ROM loader instead (slightly slower, no RAM upload) if you want to avoid the stub.

## Execution Tracing and Profiling

`--trace FILE` writes a per-task execution trace in Chrome Trace Event format.
It needs `--elf`: the FreeRTOS `pxCurrentTCBs` symbol is what makes context
switches visible.

```sh
esp-emu --chip esp32c6 --firmware build/merged.bin \
        --elf build/app.elf --trace run.json

# Timeline: drag run.json into https://ui.perfetto.dev (or chrome://tracing)
# Summary tables on localhost:
python3 tools/trace-view.py run.json 8081
```

The timeline gives one swimlane per task, so a context switch is a slice
boundary. The local page covers what a timeline is bad at — CPU time per task and
the hot-function table from sampled PCs:

```
TASK                    CPU%   TIME(ms)  SLICES
IDLE                  77.69%    4111.13     261
main                  20.67%    1093.71      89
wifi                   1.52%      80.41      54
```

Unlike SEGGER SystemView or ESP-IDF's `app_trace`, **nothing is instrumented in
the guest**: there is no Kconfig to enable, no rebuild, and no CPU or RAM cost
inside the firmware, because the emulator reads the scheduler state directly.
The trade is resolution — sampling happens on the 256-cycle peripheral tick, so
slice boundaries are accurate to ~1.6 µs at 160 MHz and a task that runs for
less than one tick can be missed. It answers "where did the time go", not
"exactly when did this switch happen".

`--trace-pc-period N` sets the PC sampling rate in ticks (default 16, `0`
disables). A trace from a run that was killed or timed out still loads — the
format permits an unterminated array.

## Building Firmware Images

The emulator runs merged flash binaries built with ESP-IDF. Requires ESP-IDF environment (`source export.sh`).

```sh
cd your-esp-idf-project
idf.py set-target esp32c3    # or esp32c5, esp32c6, esp32h2, esp32p4, esp32s3
idf.py build
idf.py merge-bin -o build/merged_flash.bin
```

## Releases

Binary tarballs (single-file artifacts containing only the `esp-emu` executable) are published as [GitHub Releases](https://github.com/espressif/esp-emulator/releases). Each release ships these assets:

- `esp-emu-<version>-x86_64-unknown-linux-gnu.tar.gz` — Linux x86_64 native
- `esp-emu-<version>-aarch64-unknown-linux-gnu.tar.gz` — Linux arm64 native
- `esp-emu-<version>-aarch64-apple-darwin.tar.gz` — macOS Apple Silicon native
- `esp-emu-<version>-wasm.tar.gz` — browser WebAssembly bundle
- `SHA256SUMS` — sha256 for each tarball; verified automatically by `install.sh`

Per-version release notes are the body of each [GitHub release](https://github.com/espressif/esp-emulator/releases).

## Docs

User guides live at [`docs/guides/`](docs/guides/) and track the latest release:

- [`BROWSER.md`](docs/guides/BROWSER.md) — running the WASM build in a browser
- [`MATTER.md`](docs/guides/MATTER.md) — Matter / Thread testing
- [`HOSTED.md`](docs/guides/HOSTED.md) — ESP-Hosted P4↔C6 setup
- [`THREAD.md`](docs/guides/THREAD.md) — 802.15.4 / OpenThread
- [`RAINMAKER.md`](docs/guides/RAINMAKER.md) — RainMaker provisioning flow

## Helper tools

Companion scripts live at [`tools/`](tools/):

- `setup-tap.sh` — create the `tap0` device for TAP networking on Linux
- `bumble_test.py` — virtual BLE controller (Google Bumble) over TCP for HCI testing
- `ws-net-proxy.py` — WebSocket↔raw-Ethernet bridge for the browser build
- `vhci_bridge.py` — Linux VHCI HCI bridge
- `make-efuse.py` — build an `--efuse` image that reports a given chip revision (`--chip esp32c3 --chip-rev 1.1`)
- `trace-view.py` — serve CPU-per-task and hot-function tables from a `--trace` file

## Manual install (without install.sh)

```sh
# Download the asset for your platform from
# https://github.com/espressif/esp-emulator/releases/latest
tar -xzf esp-emu-<version>-<platform>.tar.gz
cp esp-emu-<version>-<platform>/esp-emu ~/.local/bin/
~/.local/bin/esp-emu --help
```
