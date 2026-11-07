# Sensor board: an I2C part in the emulator

ESP32-C3 firmware reading an SHT31 temperature and humidity sensor over I2C,
run by `esp-emu` with no hardware and no browser. The sensor is
[`examples/parts/sht31`](../../parts/sht31) compiled to WebAssembly;
`board.json` wires its `SCL` to GPIO 6 and its `SDA` to GPIO 5. The firmware
uses the stock `esp_driver_i2c`; it does not know the sensor is virtual.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`sensor/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: one SHT31 at 0x44 on GPIO 6 (SCL) and 5 (SDA) |
| `sht31.wasm` | the part |
| `i2c_sensor-esp32c3.bin` | the firmware: [`test_apps/i2c_sensor`](../../../test_apps/i2c_sensor) for esp32c3 |

In that folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware i2c_sensor-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware scans the bus and finds the sensor alone, `SCAN: port=0
found=0x44`, reads it twice, prints `I2C_SENSOR: done`, then prints a reading
every second: `SENSOR: t=22.50 rh=45.00`. In a second terminal, connect to the
emulator's control channel:

```sh
nc 127.0.0.1 5556
```

and type this line, followed by Enter:

```text
part sensor input warmer
```

It answers `ok`, and the next reading is a degree higher. `cooler`, `wetter`
(+5 %RH) and `drier` work the same way. Ctrl-C closes `nc`; the emulator keeps
running.

From a script, send one command and exit once it is answered:

```sh
printf 'part sensor input warmer\n' | nc -N 127.0.0.1 5556
```

On macOS leave out `-N`: macOS's `nc` closes its side at the end of the input
by itself, and reads `-N` as another option.

## What you can send

One command a line, one answer a line:

| Command | Answer |
|---|---|
| `part list` | `ok sensor` |
| `part sensor input warmer`, `cooler`, `wetter`, `drier` | `ok` |
| `part sensor input warmer after MS` | `ok`; handed over MS of emulated time from now |
| `part sensor input set:temperature=30.5`, `set:humidity=60` | `ok`; that value, in one step |
| `part clock` | `ok MS`: emulated time |

Anything else answers `err: ...`. `--cosim-log events.jsonl` records every
transaction between the chip and the part, one JSON line each: the request
with the bytes the firmware wrote, and the sensor's answer.

## Build the pieces yourself

- The part: [`examples/parts/sht31`](../../parts/sht31) says how; the build
  gives `sht31.wasm`.
- The firmware, with ESP-IDF: in `test_apps/i2c_sensor`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put both next to `board.json`, the firmware as `i2c_sensor-esp32c3.bin`, and
run as above. Built for another chip, the firmware reads a sensor on every I2C
port it has: port 0 on SCL 6 / SDA 5 (on ESP32-P4, SCL 32 / SDA 31), port 1 on
SCL 10 / SDA 11, so a board for it wires one sensor to each, with a second
`--chip`. To write a part of your own, see
[docs/guides/PARTS.md](../../../docs/guides/PARTS.md).
