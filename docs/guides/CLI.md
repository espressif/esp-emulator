# CLI Reference and Configuration

Every `esp-emu` flag, and the emulator-side settings firmware cannot choose for itself: flash size, PSRAM size, I2C devices and the chip revision the eFuses report. The [README](../../README.md) covers the common flags; this page is the full list.

## CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `--chip <CHIP>` | (required) | Target chip: `esp32c3`, `esp32c5`, `esp32c6`, `esp32h2`, `esp32p4`, `esp32s3`, or `esp32s31` |
| `--firmware <PATH>` | (required, except in download mode) | Path to merged flash image: raw, or Intel HEX from `esptool merge-bin --format hex` (detected from the contents) |
| `--rom <PATH>` | embedded | Path to ROM ELF file (overrides the built-in default for `--chip`) |
| `--core-rom <NAME=PATH>` | embedded | ROM ELF of a core that boots its own mask ROM, replacing the embedded one. Repeatable. A `NAME` the chip has no core for is an error; no chip has such a core yet. |
| `--elf <PATH>...` | — | Application ELF(s) for BLE/Thread interception, panic intercepts and symbolised backtraces (e.g. `build/project.elf`). At app start the emulator reads the ELF SHA-256 the running app's descriptor records and uses the ELF it names, so pass both builds for an OTA test (`--elf old.elf new.elf`) and the new build's ELF takes over at the reboot. A `strip --strip-debug` copy of the build is recognised by its app descriptor and used with a warning; an ELF from some other build stays out, with a warning. |
| `--no-panic-intercept` | off | With `--elf`, don't stop at the first firmware panic: the firmware's own panic handler runs and reboots or halts per its sdkconfig. For firmware that panics on purpose (task-watchdog reset tests, deliberate `abort()`); backtraces and BLE interception stay. |
| `--efuse <PATH>` | — | Path to eFuse binary (336 bytes, QEMU-compatible). State is always saved back on exit (eFuses are one-time-programmable). |
| `--timeout <DURATION>` | — | Exit after duration (e.g. `5s`, `500ms`) |
| `--exit-on <STRING>` | — | Exit with code 0 when UART output contains this string |
| `--batch-size <N>` | `50000` | Instructions per iteration |
| `--skip-bootloader` | off | Load app directly from partition table, skipping 2nd-stage bootloader (firmware-entry path; only meaningful with `--skip-rom`). Picks the app the IDF bootloader would: the OTA slot `otadata` selects, else `factory`, else `ota_0`, probing the other slots when the chosen one is empty |
| `--skip-rom` | off | Reset the CPU at the firmware/bootloader entry instead of the real mask ROM. Default executes the ROM, which is required for KEY_SOURCE_KEY_MGR flash-key recovery and other eFuse-driven cold-boot decisions. |
| `--save-state` | off | Save flash state to disk on exit (overwrites firmware file, in the format it was read in) |
| `--inject <DATA>` | — | Payload to inject into UART RX (supports `\n` and `\r` escapes). Repeatable. |
| `--inject-on <STRING>` | — | Trigger string in UART output that causes injection (paired with `--inject`). Repeatable. |
| `--net <BACKEND>` | `tap0` if present and holding the gateway address on Linux, else `user` | Network backend: `user` (slirp-style, any OS), `user,hostfwd=tcp::H-:G,…`, `user,restrict=yes`, `user,dns=1.1.1.1`, `user,mdns-nat=yes` (Matter / mDNS service NAT), `user,net=10.0.2.0/24` (the subnet: gateway and DNS `.1`, firmware `.2`; default `192.168.4.0/24`, deprecated), `tap,ifname=tap0[,net=CIDR]` (Linux), `vmnet` (macOS) |
| `--wifi-ssid <SSID>` | `myssid` | WiFi soft AP SSID broadcast to firmware |
| `--wifi-password <PASS>` | `mypassword` | Passphrase (8-63 chars) for WPA2-PSK and WPA3-SAE. Empty string for open mode |
| `--wifi-auth <MODE>` | `wpa2-wpa3` | Soft AP security: `wpa2-wpa3` (transition), `wpa3-sae` (WPA3 only, PMF required), `wpa2-psk`, `open`, `wpa2-enterprise`, `wpa3-enterprise` |
| `--wifi-radius <HOST:PORT,SECRET>` | — | RADIUS server the enterprise modes relay EAP to |
| `--wifi-sae-pwe <PWE>` | `both` | SAE password element the AP accepts: `both`, `h2e`, `hunt-and-peck` |
| `--csi-replay <FILE>` | — | Recorded Wi-Fi CSI to play into firmware that calls `esp_wifi_set_csi(true)`, one record per received frame; without it such firmware gets a synthetic flat channel. `--csi-replay-loop` restarts the recording when it runs out. See [Wi-Fi CSI](NETWORKING.md#wi-fi-csi). |
| `--ble-hci <BACKEND>` | — | BLE HCI backend: `tcp:host:port` for a [virtual controller](BLE.md#virtual-controllers), `hci0` for a [Linux adapter](BLE.md#physical-adapter-linux). Requires `--elf`. |
| `--thread-sim <SPEC>` | — | IEEE 802.15.4 / Thread bridge, e.g. `bind:9001,peer:127.0.0.1:9002`. Forwards radio frames over localhost UDP to another emulator instance. Requires `--elf`. |
| `--uart-tcp <HOST:PORT>` | — | Bridge UART0 to a TCP server (e.g. `127.0.0.1:5555`); esptool connects via `socket://`. Mirrors QEMU's `-serial tcp::PORT,server,nowait`. While active, UART0 RX comes from the socket and UART0 TX goes to it, whichever console the firmware selects (stdin disconnected); a USB-Serial/JTAG console still prints to stdout, so a binary UART0 peer and USB logging work in one session. With `--strap-mode 0x04` the socket is the USB-Serial/JTAG download port instead. |
| `--uart1-tcp <HOST:PORT>` | — | Bridge UART1 to a TCP server. Side channel for simulating an external serial device (sensor, GPS, modem) with a host-side script — UART0 keeps stdin/stdout and `--exit-on`/`--inject-on`. Without a connected client, UART1 TX is discarded. |
| `--psram-size <SIZE>` | `8M` (C5, S3), `16M` (P4, S31) | External RAM the board is modelled with: `4M`/`8M`/`16M`/`32M`/`64M`, or `0` for no device. Only the chips with external RAM take it. Firmware cannot choose — see [PSRAM Size](#psram-size). |
| `--strap-mode <HEX>` | — | GPIO_STRAP value at reset. `0x02` = UART download mode (jumps to ROM entry instead of firmware entry; remapped per chip, so pass `0x02` on every target); `0x04` = USB-Serial-JTAG download; `0x08` = SPI flash boot (default). Mirrors QEMU's `-global driver=esp32cN.gpio,property=strap_mode,value=…`. |
| `--control-tcp <HOST:PORT>` | — | Host control channel: newline-delimited `reset`, `reset --soft`, `en low`, `en high`, `erase-flash`, `erase-region OFF LEN`, `write-region OFF FILE`, `read-region OFF LEN FILE`, `efuse-load FILE`, `ping`, `help`, and with `--cosim-board` the [`part` commands](PARTS.md#drive-the-parts-while-the-firmware-runs). Acts on the running machine, so one process and one output stream survive a reset — see [Host Control Channel](HOST-INTERFACES.md#host-control-channel). |
| `--gdb <PORT>` | — | Serve a GDB remote stub on this port (QEMU's `-s` is 1234). `riscv32-esp-elf-gdb` / `xtensa-esp32s3-elf-gdb` connect with `target remote :PORT`. On dual-core chips each core is a thread (`Thread 1` = CPU0, `Thread 2` = CPU1), so `info threads`, `thread N` and breakpoints hit on CPU1 work as under QEMU. A firmware panic or CPU fault stops the target with `SIGSEGV` for inspection instead of exiting. |
| `--gdb-halt` | off | Hold the CPU at the reset vector until a GDB client connects (QEMU's `-S`). |
| `--cosim-board <FILE>` | — | Run the `.wasm` parts a JSON board file places, inside the emulator. See [Run parts in the command-line emulator](PARTS.md#run-parts-in-the-command-line-emulator). |
| `--cosim-tcp <HOST:PORT>` | — | Serve the co-simulation bus on a socket, for device models that are not `.wasm` parts; goes with `--cosim-board`. See [Co-simulation over TCP](HOST-INTERFACES.md#co-simulation-over-tcp). |
| `--cosim-log <FILE>` | — | With `--cosim-board`: write every message between the chip and the parts to FILE, one JSON line each, as it happens. `--cosim-log-filter KINDS` keeps only the kinds named, comma-separated, such as `GpioLevel`, `GpioDrive`, `BusRequest`, `BusReply`, `PulseDrive`, `GpioEdges` or `ChipReset` (the full list: [PARTS.md](PARTS.md)); `ClockSync`, `I2sOut`, `I2sIn` and `CamFrame` are left out unless named. |
| `--trace <FILE>` | — | Write a per-task Chrome Trace Event trace for Perfetto; requires `--elf`. `--trace-pc-period N` sets the PC sample rate for the hot-symbol table (default 16 ticks, 0 disables). See [Execution Tracing and Profiling](HOST-INTERFACES.md#execution-tracing-and-profiling). |
| `--log-file <FILE>` | — | Append the emulator's log lines to FILE instead of writing them to stderr; the firmware console stays on stdout. For a harness that reads stdout and stderr as one stream, where a log line written while the firmware is partway through a line otherwise lands inside it. The startup banner and fatal errors still go to stderr. |
| `--sd <FILE[,ro][,slot=N\|,cs=PIN][,cd=PIN]>` | — | Put an SD card in the SDMMC host's slot (ESP32-P4, ESP32-S3, ESP32-S31), or with `cs=PIN` on an SPI bus of any chip: `FILE` is a raw card image. See [SD Card](#sd-card). |
| `--i2c-dev <PORT:ADDR[,OPTS]>` | — | Put an I2C device at 7-bit `ADDR` on HP I2C port `PORT`, beside the built-in EEPROM at 0x50 (one at 0x50 answers in its place: `--i2c-dev 0:0x50,width=2` is the 24C32 IDF's `i2c_eeprom` example expects). It ACKs and reads back what was written, so a board's IO expander, touch controller, PMIC or sensor gets past init. Repeatable. Options: `width=2` (16-bit register addresses), `fill=0xNN` (initial register value, default 0xFF), `reg=REG=VAL` (preload a register, e.g. a chip ID). On a port a virtual part is wired to, parts alone answer. See [I2C Devices](#i2c-devices). The same device is a `devices` entry of type `regfile` in a `--cosim-board` file ([PARTS.md](PARTS.md#run-parts-in-the-command-line-emulator)). |
| `--rmt-loopback <TX:RX>` | — | Feed an RMT TX channel's symbols into an RX channel, standing in for a wire between two pins (runs the stock `ir_nec_transceiver` example). Several pairs are comma-separated; channel numbers are the chip's own, so RX starts at 2 on C3/C6/H2 and at 4 on P4/S31. TX and RX on the same pin need no flag. The same wire is a `wires` entry of type `rmt-loopback` in a `--cosim-board` file. |
| `--i2s-wav <FILE>` | — | Record what an I2S unit transmits to a WAV file, whether or not a receiver listens: a speaker amplifier on the data pad. The header carries the sample rate, channels and width the firmware programmed. |
| `--i2s-wav-in <FILE>` | — | Feed a WAV file to an I2S unit's receiver, as a microphone on the data pad would. The file's sample rate, channels and width must be what the firmware programs for the receiver; a mismatch is refused, not resampled. Silence follows the end of the file. |
| `--i2s-instance <N>` | `0` | The I2S unit `--i2s-wav` and `--i2s-wav-in` attach to. |
| `--part-wav <NAME=FILE>` | — | With `--cosim-board`: record the sound part `NAME` makes (`part_audio_out`, an amplifier's speaker) to a WAV file, mono 16-bit at the rate the part reports. Repeatable. |
| `--part-wav-in <NAME=FILE>` | — | With `--cosim-board`: feed part `NAME` the sound it hears (`part_audio_in`, a microphone's room) from a WAV file, mono 16-bit at the rate the part hears at, in emulated time; a file at another rate is refused once that rate is known. Silence follows the end of the file. Repeatable. |
| `--csi-fps <N>` | `30` | Frames a second the MIPI CSI host's test pattern arrives at (ESP32-P4), 1 to 240. |
| `--csi-frame <FILE>` | — | Write the last frame the MIPI CSI bridge delivered to memory to FILE as a PPM, at exit (ESP32-P4): what the firmware received, RAW demosaicked and YUV converted. |
| `--csi-sensor <NAME[@ADDR]>` | — | Answer a MIPI camera sensor's SCCB probe on every I2C port (ESP32-P4): `sc2336`, `ov2710`, `ov5647` or `sc202cs`, optionally at another 7-bit address. A register file with the sensor's chip ID, so `esp_cam_sensor` finds it; the pixels are the CSI test pattern. |
| `--camera-frames <PATH>` | — | Feed the DVP camera receiver (ESP32-S3, P4, S31) frames from a directory of frame files in name order, one file, or an MJPEG stream; the sequence loops. A frame is queued only while the receiver captures, so while it keeps capturing the nth frame the firmware receives is the nth of the sequence. Frames queued when it stops for a frame period, or loses its clock, are dropped. |
| `--camera-format <FORMAT>` | `jpeg` | What `--camera-frames` holds as the sensor drives it: `jpeg`, `rgb565` (high byte first), `rgb565x` (low byte first), `uyvy`, `yuyv`, `yvyu`, `vyuy` or `grey`. No receiver register says it. JPEG files are decoded into a raw format when one is named. |
| `--camera-size <WxH>` | — | Frame size for raw `--camera-frames` files, which carry no header. With JPEG files it is checked against each picture and never used to rescale one. |
| `--camera-fps <FPS>` | `15` | Frames a second of emulated time for `--camera-frames`. |
| `--part-video <NAME=SRC>` | — | Show a `--cosim-board` part pictures (`part_video_in`), repeatable: a camera part sends them as its frames. SRC is a JPEG file, a folder of JPEG files or an MJPEG file (each looped), or `-` for an MJPEG stream on stdin, such as a camera on this machine: `ffmpeg -f avfoundation -i 0 -f mjpeg - \| esp-emu … --part-video camera=-` (`-f v4l2 -i /dev/video0` on Linux); the console then takes no input from stdin. Before each picture the part is asked what it wants, and gets it at its rate in emulated time: the JPEG as it is in a JPEG mode, else the picture as RGB888 scaled to its size. What a part refuses is logged once per reason. |
| `--camera-mjpeg <FILE>` | — | Write the JPEG frames the camera receiver delivers to one MJPEG file, as the firmware received them: a frame its descriptor chain cut short is written short. `--camera-frames` replays the file. |
| `--camera-png <DIR>` | — | Write every frame the camera receiver delivers to DIR as `frameNNNN.png`, raw and JPEG alike; the part of a short raw frame that never arrived is black. Stops after 500 frames. |
| `--console <MODE>` | `auto` | Console that stdin/stdout (and `--exit-on`/`--inject-on`) follow; under `--uart-tcp` (outside download mode) only a USB-Serial/JTAG or USB CDC-ACM console reaches stdout, UART0 being the socket's. `auto`: UART0 until the firmware selects USB-Serial/JTAG in the ROM console selector (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` builds), then EP1 — IDF's default "UART0 + USB-Serial/JTAG secondary" prints once either way. In `auto` a TinyUSB CDC-ACM console on a USB-OTG port (S3, P4, S31) joins the output and takes stdin once it has printed, unless a UART driver (a REPL) is reading UART0. `uart0`: always UART0; EP1 and CDC output discarded. `usb-jtag`: EP1 from the first byte, UART0 and CDC output discarded — needed for firmware that drives USB-Serial/JTAG outside the IDF console layer (Zephyr, esp-hal, Arduino HWCDC), which never writes the selector. `usb-cdc`: the first CDC-ACM port the host configures, alone, until it goes down; UART0, EP1 and other ports' output discarded, and stdin typed before a port is up is held for it. Refused on chips without USB-OTG and in download mode. |
| `--h264-dump <FILE>` | — | Write every frame the H.264 encoder produced (ESP32-P4) to FILE on exit, as one Annex-B stream. |
| `--hosted <SPEC>` | — | ESP-Hosted SDIO bridge: `bridge:host:/sock` (ESP32-P4 host, connects) or `bridge:slave:/sock` (ESP32-C5/C6 slave, listens; start it first). See [HOSTED.md](HOSTED.md). |
| `--hosted-reset-gpio <PIN>` | — | Host side: the P4 GPIO wired to the slave's EN pin. When the firmware pulses it, the slave instance reboots. Must match the firmware's reset-GPIO Kconfig. |
| `--hosted-reset-active-high` | off | Treat the slave-reset GPIO as active-high (default: active-low). |
| `--wifi-fw-ap-password <PASS>` | `--wifi-password` | Password of the firmware's own soft AP, used when the emulator joins it; `""` for an open AP. See [NETWORKING.md](NETWORKING.md). |
| `--log-color <WHEN>` | `auto` | Colour the firmware's `I/W/E (..)` log lines on stdout: `auto` (a terminal, and `NO_COLOR` unset), `always` or `never`. |
| `--config <PATH>` | `~/.esp-emu/settings.json` | Settings file to read (see below). A path given here must exist. |

The co-simulation flags (`--cosim-board`, `--cosim-tcp`, `--cosim-log`, `--cosim-log-filter`, `--part-video`, `--part-wav`, `--part-wav-in`) are in the release binaries. If you build the emulator from its source, add `--features cosim-parts` (`cosim` for `--cosim-tcp` alone).

## Subcommands

| Command | What it does |
|---------|--------------|
| `esp-emu update` | Install the latest release in place of this binary. `--check` only reports, `--version X.Y.Z` picks a release, `--force` reinstalls. |
| `esp-emu config autoupdate [on\|off]` | Read or set the background self-update, off by default. When on, the emulator looks for a newer release at start-up and installs it in the background; the new build takes effect on the next run. `ESP_EMU_DISABLE_AUTOUPDATES=1` always turns it off, and a binary run from a cargo `target/` tree never updates itself. |
| `esp-emu config path` | Print the settings file in use. |

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

C5 and S3 stay at 8 MB, the largest module shipped for either, because their
flash and PSRAM share one 32 MB address window: a die that fills it leaves
`esp_partition` no page to map the partition table, and firmware fails at boot
with `load_partitions returned 0x101` (`CONFIG_SPI_FLASH_ROM_IMPL`) or `0x105`.

`--psram-size 0` fits no die at all. The driver's connectivity check then fails
and firmware takes its not-found path — `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` boots
anyway, without it `esp_psram` aborts, which is exactly what the hardware does
with an unpopulated footprint.

A size no device ID can name is refused at startup rather than answered wrong:

```
$ esp-emu --chip esp32p4 --firmware app.bin --psram-size 12M
Error: esp32p4 cannot report a 12582912-byte PSRAM; its device ID names 4M, 8M, 16M, 32M, 64M (or 0 for none)
```

## SD Card

`--sd FILE` puts an SD card in the SDMMC host of an ESP32-P4, ESP32-S3 or
ESP32-S31, so the stock `esp_vfs_fat_sdmmc_mount` mounts it. `--sd
FILE,cs=PIN` puts it on an SPI bus instead, on any chip, for
`esp_vfs_fat_sdspi_mount`. `FILE` is a raw image of the card: a FAT volume,
with a partition table or without one.

```sh
truncate -s 64M card.img && mkfs.vfat -F 32 card.img
esp-emu --chip esp32p4 --firmware build/merged_flash.bin --sd card.img
```

- The card is there only when the flag is given. Without it the slot is empty
  and every command times out.
- It is an SDHC card of the image's size, in 512-byte blocks. The size counts
  in units of 512 KiB; a remainder is not part of the card.
- What the firmware writes goes into the image as it is written, so the image
  can be prepared before a run and inspected after it.
- `--sd card.img,ro` write-protects the card. The image is opened read-only
  and the driver's writes fail, as on a protected card.
- `--sd card.img,slot=1` names the slot. Without it the card is in the slot
  the firmware brings a memory card up on; beside an ESP-Hosted slave
  (`--hosted`), which takes slot 1, it is in slot 0.

- `--sd card.img,cs=10` is a card in SPI mode: `PIN` is the GPIO the firmware
  drives as the card's chip select (`sdspi_device_config_t.gpio_cs`). The card
  takes what either GP-SPI bus clocks while that pad is low, so the bus and
  the other three pads need no naming. `slot=` and `cs=` exclude each other.

- `--sd card.img,cd=21` names the GPIO the slot's card-detect switch is on:
  the pad is held low while the card is there, for firmware that passes
  `gpio_cd`. On the SDMMC host a card-detect or write-protect pad the firmware
  routes reads as that pad's level, so a script or a part can move it.

Removing the card during a run is not modelled. In a `--cosim-board`
file the same card is a `devices` entry:
`{ "type": "sdcard", "image": "card.img", "read_only": false, "slot": 0 }`,
or with `"cs": 10` in place of `"slot"`.

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

The same devices as a board file, which keeps a board's parts, devices and
wires in one place
([PARTS.md](PARTS.md#run-parts-in-the-command-line-emulator)):

```json
{ "devices": {
    "expander": { "type": "regfile", "i2c": 0, "addr": "0x20" },
    "touch":    { "type": "regfile", "i2c": 0, "addr": "0x5a", "width": 2, "fill": 0, "regs": { "0xd006": "0xab" } },
    "pmic":     { "type": "regfile", "i2c": 0, "addr": "0x34", "regs": { "0x03": "0x4a" } }
} }
```

`PORT` is the IDF `i2c_port_t`. `width=2` selects 16-bit (big-endian)
register addresses. `fill` is what every register reads until written
(default 0xFF, like a pulled-up, unprogrammed part). `reg=REG=VAL` preloads
one register — a chip ID the driver checks, a status byte it polls for.

The device has no behaviour beyond read-back: an expander's input port does
not follow its output latch or the pin level, and a sensor's data registers
hold whatever was preloaded. A device with behaviour of its own is a virtual
part ([PARTS.md](PARTS.md)); on a port a part is wired to, parts alone answer,
and neither the built-in EEPROM nor an `--i2c-dev` device is asked.

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
