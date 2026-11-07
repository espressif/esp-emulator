# Thermometer board: an SPI part in the emulator

ESP32-C3 firmware reading a TMP123 temperature sensor over SPI, run by
`esp-emu` with no hardware and no browser. The sensor is
[`examples/parts/tmp123`](../../parts/tmp123) compiled to WebAssembly;
`board.json` wires its chip select `CS` to GPIO 7, its clock `SCK` to GPIO 6
and its data output `SO` to GPIO 2. The firmware uses the stock
`esp_driver_spi`; it does not know the sensor is virtual.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`thermometer/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: one TMP123 with its chip select on GPIO 7 |
| `tmp123.wasm` | the part |
| `spi_tmp123-esp32c3.bin` | the firmware: [`test_apps/spi_tmp123`](../../../test_apps/spi_tmp123) for esp32c3 |

In that folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware spi_tmp123-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware reads the sensor three times: polling for the transfer's end,
sleeping until its interrupt, and through DMA, as most drivers read
(`SPI_DMA_CH_AUTO`), `TMP123: host=2 polling raw=0x0C80 t=25.0000`. It prints
`SPI_TMP123: done`, then a reading every second: `THERMO: t=25.0000`. In a second terminal, connect to the emulator's control
channel:

```sh
nc 127.0.0.1 5556
```

and type this line, followed by Enter:

```text
part thermometer input warmer
```

It answers `ok`, and the next reading is a degree higher. `cooler` works the
same way. Ctrl-C closes `nc`; the emulator keeps running.

## What you can send

One command a line, one answer a line:

| Command | Answer |
|---|---|
| `part list` | `ok thermometer` |
| `part thermometer input warmer`, `cooler` | `ok` |
| `part thermometer input warmer after MS` | `ok`; handed over MS of emulated time from now |
| `part thermometer input set:temperature=30.5` | `ok`; that value, in one step |
| `part clock` | `ok MS`: emulated time |
| `part pending` | `ok N`: inputs not handed over yet |

Anything else answers `err: ...`. The input name goes to the part as typed,
and the part ignores a name it does not have, so a misspelt `warmer` answers
`ok` and does nothing. `--cosim-log events.jsonl` records every
transfer between the chip and the part, one JSON line each: the request, with
its bus, chip-select slot, framing and the pads it was clocked on, and the
sensor's answer.

## Build the pieces yourself

- The part: [`examples/parts/tmp123`](../../parts/tmp123) says how; the build
  gives `tmp123.wasm`.
- The firmware, with ESP-IDF: in `test_apps/spi_tmp123`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put both next to `board.json`, the firmware as `spi_tmp123-esp32c3.bin`, and
run as above. Built for a chip with a second SPI bus (ESP32-S3, P4, S31), the
firmware also reads a sensor on SPI3, with its chip select on GPIO 15, its
clock on GPIO 5 and its data output on GPIO 4, so a board for it wires a
second TMP123 there. To write a part of your own, see
[docs/guides/PARTS.md](../../../docs/guides/PARTS.md).
