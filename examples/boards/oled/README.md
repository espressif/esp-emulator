# OLED board: a screen you can read back

ESP32-C3 firmware drawing on an SSD1306 128 x 64 OLED module over I2C and
redrawing it when a button is pressed, run by `esp-emu` with no hardware and
no browser. The module is [`examples/parts/ssd1306`](../../parts/ssd1306) and
the button [`examples/parts/pushbutton`](../../parts/pushbutton), both
compiled to WebAssembly; `board.json` wires the module's `SCL` to GPIO 6 and
`SDA` to GPIO 5, and the button to GPIO 9. The firmware drives the module
with the stock `esp_driver_i2c` and the button with a GPIO interrupt; it does
not know either is virtual. What the screen shows is read back through the
module's `part_render`.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`oled/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: the module on GPIO 6 and 5, the button on GPIO 9 |
| `ssd1306.wasm` | the module |
| `pushbutton.wasm` | the button |
| `oled_button-esp32c3.bin` | the firmware: [`test_apps/oled_button`](../../../test_apps/oled_button) for esp32c3 |

In that folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware oled_button-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware draws `HELLO` over `READY`, prints `OLED: ready`, then
`BUTTON: ready level=1`. In a second terminal, this prints the screen, a
character a pixel:

```sh
printf 'part oled render\n' | nc -N 127.0.0.1 5556 | python3 -c '
import sys
b = bytes.fromhex(sys.stdin.read().split()[1])
for y in range(64):
    print("".join("#" if b[y * 16 + x // 8] >> (7 - x % 8) & 1 else "." for x in range(128)))'
```

On macOS leave out `-N`: its `nc` stops at the end of its input without it,
and its `-N` means something else. Then press the button:

```sh
printf 'part button input press\n' | nc -N 127.0.0.1 5556
```

The firmware prints `BUTTON: pressed` and draws `BUTTON` over `PRESSED`; the
command above prints it. Asked again before the screen changes,
`part oled render` answers `ok` alone: the module hands out a picture only
when it changed, and whoever draws keeps the last one.

## What you can send

One command a line, one answer a line:

| Command | Answer |
|---|---|
| `part list` | `ok oled button` |
| `part oled render` | `ok HEX`: the screen, 1024 bytes in hex, a bit a pixel, rows top to bottom, 16 bytes a row, the most significant bit leftmost, 1 lit; `ok` alone when nothing changed since the last ask |
| `part oled state` | `ok format=mono height=64 inverted=false on=true width=128` |
| `part button input press`, `part button input release` | `ok`; the button pressed or let go |
| `part button state` | `ok pressed=false` |
| `part clock` | `ok MS`: emulated time |

Anything else answers `err: ...`. `--cosim-log events.jsonl` records every
message between the chip and the parts, one JSON line each: here the
firmware's I2C transfers to the module, a full frame each time it draws.

## Build the pieces yourself

- The parts: [`examples/parts/ssd1306`](../../parts/ssd1306) and
  [`examples/parts/pushbutton`](../../parts/pushbutton) say how; the builds
  give `ssd1306.wasm` and `pushbutton.wasm`.
- The firmware, with ESP-IDF: in `test_apps/oled_button`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put them next to `board.json`, the firmware as `oled_button-esp32c3.bin`, and
run as above. The firmware also builds for ESP32-S3, on the same pins, and
for ESP32-P4, with the module on SCL 32 and SDA 31; change `board.json` to
match and pass that chip to `--chip`.
