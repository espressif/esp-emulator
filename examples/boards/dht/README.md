# DHT board: a temperature and humidity sensor read by timing one wire

ESP32-C3 firmware reading a DHT22 temperature and humidity sensor, run by
`esp-emu` with no hardware and no browser. The sensor is
[`examples/parts/dht22`](../../parts/dht22) compiled to WebAssembly;
`board.json` wires its `DATA` to GPIO 5. The firmware pulls the line low,
lets go, and times every level of the sensor's reply itself with the stock
GPIO driver; it does not know the sensor is virtual.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`dht/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: one DHT22 with `DATA` on GPIO 5 |
| `dht22.wasm` | the part |
| `dht22_read-esp32c3.bin` | the firmware: [`test_apps/dht22_read`](../../../test_apps/dht22_read) for esp32c3 |

In that folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware dht22_read-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware prints `DHT22: ready pin=5`, then reads the sensor every half
second and prints each reading:
`DHT22: read round=0 mode=od rh=45.0 t=23.5 lo=80 hi=80`. The first two
reads drive the pin open-drain (`od`), the rest push-pull (`pp`); `lo` and
`hi` are the two halves of the sensor's presence pulse as the firmware
measured them, in microseconds.

To change the reading, connect to the emulator's control channel in a second
terminal:

```sh
nc 127.0.0.1 5556
```

and type, each followed by Enter:

```text
part dht input set:temperature=-3.5
part dht input set:humidity=62.4
```

Each answers `ok`, and the reads after it print `rh=62.4 t=-3.5`. Ctrl-C
closes `nc`; the emulator keeps running.

## What you can send

One command a line, one answer a line:

| Command | Answer |
|---|---|
| `part list` | `ok dht` |
| `part dht input set:temperature=V` | `ok`; the temperature in degrees Celsius, -40 to 80 |
| `part dht input set:humidity=V` | `ok`; the relative humidity in percent, 0 to 99.9 |
| `part dht state` | `ok humidity=45.0 reads=4 temperature=23.5`: the next reply's values, and the replies so far |
| `part clock` | `ok MS`: emulated time |

Anything else answers `err: ...`. `--cosim-log events.jsonl` records every
message between the chip and the part, one JSON line each: here, the
firmware's start pulse on `DATA` as two edges, and each reply as one
`PulseDrive` of 83 widths.

## Build the pieces yourself

- The part: [`examples/parts/dht22`](../../parts/dht22) says how; the build
  gives `dht22.wasm`.
- The firmware, with ESP-IDF: in `test_apps/dht22_read`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put both next to `board.json`, the firmware as `dht22_read-esp32c3.bin`, and
run as above. The firmware builds for every chip (ESP32-C3, C5, C6, H2, P4,
S3 and S31) on the same pin; pass that chip to `--chip`.
