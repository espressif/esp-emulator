# Analog board: a voltage on an ADC pad

ESP32-C3 firmware reading one ADC pad, run by `esp-emu` with no hardware and
no browser, against either of two parts:

- `board.json`: a potentiometer, [`examples/parts/potentiometer`](../../parts/potentiometer),
  its wiper on GPIO 3.
- `thermistor.json`: an NTC thermistor divider,
  [`examples/parts/ntc_thermistor`](../../parts/ntc_thermistor), its output on
  GPIO 3.

GPIO 3 is ADC1 channel 3 on the C3. The firmware uses the stock `adc_oneshot`
driver at 12 dB and the chip's curve-fitting calibration; it does not know the
voltage comes from a part.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`analog/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the knob: a potentiometer with its wiper on GPIO 3 |
| `thermistor.json` | the thermistor: an NTC divider with its output on GPIO 3 |
| `potentiometer.wasm`, `ntc_thermistor.wasm` | the parts |
| `adc_oneshot-esp32c3.bin` | the firmware: [`test_apps/adc_oneshot`](../../../test_apps/adc_oneshot) for esp32c3 |

In that folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware adc_oneshot-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The knob starts half way, 1650 mV. The firmware reads it three times,
`ADC: read=0 raw=2409 mv=1640`, prints `ADC_ONESHOT: done`, then a reading
every second. The millivolts are the calibration scheme's: it takes 10 mV off
at this voltage on the C3, so 1650 reads as 1640. In a second terminal,
connect to the emulator's control channel:

```sh
nc 127.0.0.1 5556
```

and type this line, followed by Enter:

```text
part knob input more
```

It answers `ok`, and the next reading is `ADC: raw=2891 mv=1962`, the wiper at
60 %, 1980 mV. `less` turns it back 10 %, and `set:position=20` puts it at
20 %. Ctrl-C closes `nc`; the emulator keeps running.

With `--cosim-board thermistor.json` the pad carries the thermistor instead.
It starts at 25 °C, which is also 1650 mV; `part ntc input set:temperature=50`
moves the output to 871 mV, and `warmer` and `cooler` move it a degree at a
time. The voltage falls as the thermistor warms.

## What you can send

One command a line, one answer a line:

| Command | Answer |
|---|---|
| `part list` | `ok knob` (or `ok ntc`) |
| `part knob input more`, `less` | `ok` |
| `part knob input set:position=20` | `ok`; that position, in percent |
| `part ntc input warmer`, `cooler`, `set:temperature=37.5` | `ok` |
| `part knob state` | `ok` and the knob's `position` and `millivolts` |
| `part clock` | `ok MS`: emulated time |

`--cosim-log events.jsonl` records every voltage the part put on the pad, as
`AnalogDrive` with its `pin` and `millivolts`.

## Build the pieces yourself

- The parts: [`examples/parts/potentiometer`](../../parts/potentiometer) and
  [`examples/parts/ntc_thermistor`](../../parts/ntc_thermistor) say how.
- The firmware, with ESP-IDF: in `test_apps/adc_oneshot`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put them next to the board files, the firmware as `adc_oneshot-esp32c3.bin`,
and run as above. Built for an ESP32-C5, C6 or H2 the firmware reads GPIO 3 as
well (ADC1 channel 2 on the C5 and H2); for an ESP32-S31 it reads GPIO 43 at
0 dB, and for an ESP32-P4 GPIO 20 (ADC1 channel 4), so a board for either
wires the part's pin there; for an ESP32-S3 it reads GPIO 3 (ADC1 channel 2).
The H2's emulated eFuse carries no ADC calibration, so on the H2 the firmware
prints raw codes only. To write a part of your own, see [docs/guides/PARTS.md](../../../docs/guides/PARTS.md).
