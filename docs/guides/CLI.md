# CLI Reference and Configuration

Every `esp-emu` flag, and the emulator-side settings firmware cannot choose for itself: flash size, PSRAM size, I2C devices and the chip revision the eFuses report. The [README](../../README.md) covers the common flags; this page is the full list.

## CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `--chip <CHIP>` | (required) | Target chip: `esp32c3`, `esp32c5`, `esp32c6`, `esp32h2`, `esp32p4`, `esp32s3`, or `esp32s31` |
| `--firmware <PATH>` | (required) | Path to merged flash binary |
| `--rom <PATH>` | embedded | Path to ROM ELF file (overrides the built-in default for `--chip`) |
| `--elf <PATH>...` | — | Application ELF(s) for BLE/Thread interception, panic intercepts and symbolised backtraces (e.g. `build/project.elf`). At app start the emulator reads the ELF SHA-256 the running app's descriptor records and uses the ELF it names, so pass both builds for an OTA test (`--elf old.elf new.elf`) and the new build's ELF takes over at the reboot. An ELF from some other build stays out, with a warning. |
| `--no-panic-intercept` | off | With `--elf`, don't stop at the first firmware panic: the firmware's own panic handler runs and reboots or halts per its sdkconfig. For firmware that panics on purpose (task-watchdog reset tests, deliberate `abort()`); backtraces and BLE interception stay. |
| `--efuse <PATH>` | — | Path to eFuse binary (336 bytes, QEMU-compatible). State is always saved back on exit (eFuses are one-time-programmable). |
| `--timeout <DURATION>` | — | Exit after duration (e.g. `5s`, `500ms`) |
| `--exit-on <STRING>` | — | Exit with code 0 when UART output contains this string |
| `--batch-size <N>` | `50000` | Instructions per iteration |
| `--skip-bootloader` | off | Load app directly from partition table, skipping 2nd-stage bootloader (firmware-entry path; only meaningful with `--skip-rom`). Picks the app the IDF bootloader would: the OTA slot `otadata` selects, else `factory`, else `ota_0`, probing the other slots when the chosen one is empty |
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
| `--control-tcp <HOST:PORT>` | — | Host control channel: newline-delimited `reset`, `reset --soft`, `erase-flash`, `erase-region OFF LEN`, `write-region OFF FILE`, `ping`. Acts on the running machine, so one process and one output stream survive a reset — see [Host Control Channel](HOST-INTERFACES.md#host-control-channel). |
| `--gdb <PORT>` | — | Serve a GDB remote stub on this port (QEMU's `-s` is 1234). `riscv32-esp-elf-gdb` / `xtensa-esp32s3-elf-gdb` connect with `target remote :PORT`. |
| `--gdb-halt` | off | Hold the CPU at the reset vector until a GDB client connects (QEMU's `-S`). |
| `--trace <FILE>` | — | Write a per-task Chrome Trace Event trace for Perfetto; requires `--elf`. `--trace-pc-period N` sets the PC sample rate for the hot-symbol table (default 16 ticks, 0 disables). See [Execution Tracing and Profiling](HOST-INTERFACES.md#execution-tracing-and-profiling). |
| `--i2c-dev <PORT:ADDR[,OPTS]>` | — | Put an I2C device at 7-bit `ADDR` on HP I2C port `PORT`, beside the built-in EEPROM at 0x50. It ACKs and reads back what was written, so a board's IO expander, touch controller, PMIC or sensor gets past init. Repeatable. Options: `width=2` (16-bit register addresses), `fill=0xNN` (initial register value, default 0xFF), `reg=REG=VAL` (preload a register, e.g. a chip ID). See [I2C Devices](#i2c-devices). |
| `--rmt-loopback <TX:RX>` | — | Feed an RMT TX channel's symbols into an RX channel, standing in for a wire between two pins (runs the stock `ir_nec_transceiver` example). |
| `--console <MODE>` | `auto` | Console that stdin/stdout (and `--exit-on`/`--inject-on`) follow. `auto`: UART0 until the firmware selects USB-Serial/JTAG in the ROM console selector (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` builds), then EP1 — IDF's default "UART0 + USB-Serial/JTAG secondary" prints once either way. `uart0`: always UART0, EP1 output discarded. `usb-jtag`: EP1 from the first byte — needed for firmware that drives USB-Serial/JTAG outside the IDF console layer (Zephyr, esp-hal, Arduino HWCDC), which never writes the selector. |

## Log Levels

Control verbosity via `RUST_LOG`:

```sh
RUST_LOG=info  esp-emu ...   # Default, boot messages
RUST_LOG=debug esp-emu ...   # Peripheral access details
RUST_LOG=trace esp-emu ...   # Every bus read/write
```

## Flash Size

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

## PSRAM Size

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

## I2C Devices

The HP I2C controller is modelled on every chip, and the bus has one device
by default: a 24Cxx-style EEPROM at address 0x50 (an I2C scan under the
emulator finds it; it is not in your wiring). Everything else NACKs, so
firmware whose board init talks to an IO expander, touch controller, PMIC or
sensor fails its first transaction and usually aborts before `app_main`:

```
E (36) i2c.master: I2C transaction unexpected nack detected
E (36) tca95xx_16: esp_io_expander_new_i2c_tca95xx_16bit(81): Reset failed
```

`--i2c-dev` adds a register-file device at an address: it ACKs, stores what
is written and reads it back, with an auto-incrementing register pointer. That
is enough for drivers that only need the transaction to succeed. One flag per
device:

```sh
# TCA9555 IO expander at 0x20; CST9217 touch at 0x5a (16-bit registers, its
# ACK byte preloaded); AXP2101 PMIC at 0x34 with its chip ID preloaded.
esp-emu --chip esp32s3 --firmware app.bin \
  --i2c-dev 0:0x20 \
  --i2c-dev 0:0x5a,width=2,fill=0,reg=0xd006=0xab \
  --i2c-dev 0:0x34,reg=0x03=0x4a
```

`PORT` is the IDF `i2c_port_t`. `width=2` selects 16-bit (big-endian)
register addresses. `fill` is what every register reads until written
(default 0xFF, like a pulled-up, unprogrammed part). `reg=REG=VAL` preloads
one register — a chip ID the driver checks, a status byte it polls for.

The device has no behaviour beyond read-back: an expander's input port does
not follow its output latch or the pin level, and a sensor's data registers
hold whatever was preloaded. Real device models and host-side device bridges
are tracked separately.

## Chip Revision (eFuse)

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
covers C3, C5, C6, H2, P4, S3 and S31, writes the MAC and calibration defaults the
emulator would otherwise seed, and warns past each chip's `REV_MAX_FULL`, where
the bootloader rejects every image. espefuse over `socket://` cannot do this:
BLK1 is Reed-Solomon coded and the burn aborts once the seeded revision leaves
it non-empty. An `--efuse` image that states any wafer field is honoured as
written; the default is seeded only when all of them read zero.

### ESP32-P4 images built for rev 0.x / 1.x silicon

The embedded P4 ROM is the eco5 (rev 3.x) ROM, and ESP-IDF links rev 0.x/1.x
and rev 3.x bootloaders against different ROM layouts. ESP-IDF v5.4, and v5.5
up to v5.5.2, build for rev 0.x/1.x by default
(`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`); v5.5.3 and later default to rev 3.1.
Such an image would fault in the 2nd-stage bootloader, so the emulator refuses
it at startup: `esp32p4 image built for silicon v0.1..v1.99 but the loaded ROM
serves v3.0..v3.99`.

Either rebuild with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=n`, or run the image on
the rev0 ROM ELF from IDF's `esp-rom-elfs` tool with an eFuse blob to match:

```sh
python3 tools/make-efuse.py --chip esp32p4 --chip-rev 1.0 -o p4_rev1p0.efuse
esp-emu --chip esp32p4 --firmware build/merged_flash.bin \
  --rom ~/.espressif/tools/esp-rom-elfs/20241011/esp32p4_rev0_rom.elf \
  --efuse p4_rev1p0.efuse
```

`--rom` is what lifts the refusal; `--efuse` satisfies the bootloader's own
revision check (without it the emulator warns and the app is rejected). A
rev-3 image on the rev0 ROM is refused the same way.
