# esptool / espefuse over `socket://`

Drive the running emulator with `esptool.py`, `espefuse.py` and `idf.py flash` over TCP, the same way the QEMU-Espressif workflow does.

esp-emu can expose its emulated UART as a TCP server so `esptool.py`, `espefuse.py`, and `idf.py flash` can drive it via `socket://host:port`, matching the workflow documented for [QEMU-Espressif](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/esp32c3/README.md#using-esptoolpy-and-espefusepy-to-interact-with-qemu).

## Setup

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

## What works

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

## Caveats

- **`--before no-reset --after no-reset` are required.** RTS isn't transmissible over a TCP socket, so esptool can't auto-reset the target. To reboot the emulated chip without restarting the process, send `reset` over `--control-tcp` (see [Host Control Channel](HOST-INTERFACES.md#host-control-channel)).
- **`--no-stub` is an optional fallback.** Stub mode is the default and works for everything above; `--no-stub` uses the ROM loader instead (slightly slower, no RAM upload) if you want to avoid the stub.
