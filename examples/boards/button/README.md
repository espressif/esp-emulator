# Push button board: a virtual part in the emulator

ESP32-C3 firmware with a push button wired to GPIO 9, run by `esp-emu` with no
hardware and no browser. The button is
[`examples/parts/pushbutton`](../../parts/pushbutton) compiled to WebAssembly;
`board.json` wires its pin `A` to GPIO 9.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`button/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: one push button on GPIO 9 |
| `pushbutton.wasm` | the part |
| `gpio_button-esp32c3.bin` | the firmware: [`test_apps/gpio_button`](../../../test_apps/gpio_button) for esp32c3, which prints every edge of GPIO 9 |

In that folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware gpio_button-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware prints `BUTTON: ready level=1`: the pin rests high. In a second
terminal, connect to the emulator's control channel:

```sh
nc 127.0.0.1 5556
```

and type these two lines, each followed by Enter:

```text
part button input press
part button input release
```

Each answers `ok`, and the firmware prints `BUTTON: pressed`, then
`BUTTON: released`, both from its own GPIO interrupt. Ctrl-C closes `nc`; the
emulator keeps running.

From a script, send one command and exit once it is answered:

```sh
printf 'part button input press\n' | nc -N 127.0.0.1 5556
```

On macOS leave out `-N`: macOS's `nc` closes its side at the end of the input
by itself, and reads `-N` as another option.

## What you can send

One command a line, one answer a line:

| Command | Answer |
|---|---|
| `part list` | `ok button` |
| `part button input press`, `... release` | `ok` |
| `part button input release after MS` | `ok`; handed over MS of emulated time from now, so a 3000 ms hold lasts 3 s on the device's clock however fast your computer runs it |
| `part button input press at MS` | `ok`; handed over at emulated time MS |
| `part clock` | `ok MS`: emulated time |
| `part pending` | `ok N`: inputs not handed over yet |

Anything else answers `err: ...`. The input name goes to the part as typed,
and the part ignores a name it does not have, so a misspelt `press` answers
`ok` and does nothing. `--cosim-log events.jsonl` records every message
between the chip and the part, one JSON line each, stamped with emulated time.

## Build the pieces yourself

- The part: [`examples/parts/pushbutton`](../../parts/pushbutton) says how;
  the build gives `pushbutton.wasm`.
- The firmware, with ESP-IDF: in `test_apps/gpio_button`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put both next to `board.json`, the firmware as `gpio_button-esp32c3.bin`, and
run as above. To write a part of your own, see
[docs/guides/PARTS.md](../../../docs/guides/PARTS.md).
