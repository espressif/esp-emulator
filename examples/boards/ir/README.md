# IR board: a remote control decoded through RMT

ESP32-C3 firmware decoding a remote control's NEC frames from a VS1838B
infrared receiver module, run by `esp-emu` with no hardware and no browser.
The module and its remote are [`examples/parts/vs1838b`](../../parts/vs1838b)
compiled to WebAssembly; `board.json` wires its `OUT` to GPIO 4. The firmware
times the module's output with the stock `esp_driver_rmt` receiver and
decodes it as ESP-IDF's `ir_nec_transceiver` example does; it does not know
the remote is virtual.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`ir/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: one VS1838B with `OUT` on GPIO 4 |
| `vs1838b.wasm` | the part |
| `ir_nec_rx-esp32c3.bin` | the firmware: [`test_apps/ir_nec_rx`](../../../test_apps/ir_nec_rx) for esp32c3 |

In that folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware ir_nec_rx-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware prints `IR: ready pin=4` once the receiver is armed. In a second
terminal, connect to the emulator's control channel:

```sh
nc 127.0.0.1 5556
```

and type these lines, each followed by Enter:

```text
part ir input ok
part ir input left
```

Each answers `ok`. Each key is one NEC frame, and the firmware prints the
two words it decoded, the address and the command, each with its inverse in
the high byte: `IR: address=FF00 command=E31C`, then
`IR: address=FF00 command=F708`. Ctrl-C closes `nc`; the emulator keeps
running.

## What you can send

One command a line, one answer a line:

| Command | Answer |
|---|---|
| `part list` | `ok ir` |
| `part ir input KEY` | `ok`; presses `KEY`: `0` to `9`, `star`, `hash`, `up`, `down`, `left`, `right` or `ok` |
| `part ir input ok after MS` | `ok`; handed over MS of emulated time from now |
| `part ir state` | `ok address=0 command=8 frames=2 key=left` |
| `part clock` | `ok MS`: emulated time |
| `part pending` | `ok N`: inputs not handed over yet |

Anything else answers `err: ...`. The input name goes to the part as typed,
and the part ignores a name it does not have, so a misspelt key answers `ok`
and does nothing. `--cosim-log events.jsonl` records every message between
the chip and the part, one JSON line each: here, the 68 edges of each frame
on `OUT`, each stamped with the emulated time it lands.

## Build the pieces yourself

- The part: [`examples/parts/vs1838b`](../../parts/vs1838b) says how; the
  build gives `vs1838b.wasm`.
- The firmware, with ESP-IDF: in `test_apps/ir_nec_rx`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put both next to `board.json`, the firmware as `ir_nec_rx-esp32c3.bin`, and
run as above. The firmware builds for every chip (ESP32-C3, C5, C6, H2, P4,
S3 and S31) on the same pin; pass that chip to `--chip`.
