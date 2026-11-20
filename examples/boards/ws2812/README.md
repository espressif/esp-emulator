# Strip board: an LED strip driven through RMT

ESP32-C3 firmware driving a WS2812 LED strip of eight pixels with the stock
RMT transmit driver, run by `esp-emu` with no hardware and no browser. The
strip is [`examples/parts/ws2812`](../../parts/ws2812) compiled to
WebAssembly; `board.json` wires its `DIN` to GPIO 8. The firmware builds
every symbol of the strip's protocol itself and does not know the strip is
virtual; the part reads the waveform on the pin and shows each pixel's
colour.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`ws2812/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: one WS2812 strip with `DIN` on GPIO 8 |
| `ws2812.wasm` | the part |
| `ws2812_strip-esp32c3.bin` | the firmware: [`test_apps/ws2812_strip`](../../../test_apps/ws2812_strip) for esp32c3 |

In that folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware ws2812_strip-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware prints `STRIP: ready pin=8 pixels=8`, then sends a frame a fifth
of a second: two fixed patterns, then a turning rainbow, printing
`STRIP: frame 0 sent` and so on. In a second terminal, connect to the
emulator's control channel:

```sh
nc 127.0.0.1 5556
```

and type `part strip state`, followed by Enter. Asked right after the first
frame, it answers
`ok 0=#ff0000 1=#00ff00 2=#0000ff 3=#010203 4=#ffffff 5=#000000 6=#102030 7=#804007 frames=1 pixels=8`:
each pixel's colour, the frames shown so far and the pixels in the last one.
Asked later, it shows the frame the rainbow has reached. Ctrl-C closes `nc`;
the emulator keeps running.

## What you can send

One command a line, one answer a line:

| Command | Answer |
|---|---|
| `part list` | `ok strip` |
| `part strip state` | `ok 0=#RRGGBB ... frames=N pixels=8` |
| `part clock` | `ok MS`: emulated time |

Anything else answers `err: ...`. `--cosim-log events.jsonl` records every
message between the chip and the part, one JSON line each: here, each run of
edges on `DIN` (`GpioEdges`), up to 512 widths a run, a few runs a frame.

## Build the pieces yourself

- The part: [`examples/parts/ws2812`](../../parts/ws2812) says how; the build
  gives `ws2812.wasm`.
- The firmware, with ESP-IDF: in `test_apps/ws2812_strip`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put both next to `board.json`, the firmware as `ws2812_strip-esp32c3.bin`,
and run as above. The firmware builds for every chip (ESP32-C3, C5, C6, H2,
P4, S3 and S31) on the same pin; pass that chip to `--chip`. ESP-IDF's own
`examples/peripherals/rmt/led_strip` drives the same part unchanged, with
`DIN` on GPIO 0 and 24 pixels.
