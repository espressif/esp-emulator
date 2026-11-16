# Watchdog board: a part that resets the chip

ESP32-C3 firmware supervised by an external windowed watchdog, run by
`esp-emu` with no hardware and no browser. The supervisor is
[`examples/parts/watchdog`](../../parts/watchdog) compiled to WebAssembly;
`board.json` wires its `WDI` input to GPIO 4 and its `RESET` output to the
chip's EN (CHIP_PU) pin.

The firmware kicks the supervisor every 200 ms, ten times, and then hangs, as
a stuck control loop does. The supervisor pulls EN low for 200 ms: the chip is
held in reset for that long on its own clock, then boots as from power-on,
and the firmware reports the reset reason. It does not know the supervisor is
virtual.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`watchdog/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: the supervisor on GPIO 4 and EN |
| `watchdog.wasm` | the part |
| `en_watchdog-esp32c3.bin` | the firmware: [`test_apps/en_watchdog`](../../../test_apps/en_watchdog) for esp32c3 |

In that folder:

```sh
RUST_LOG=info /path/to/esp-emu --chip esp32c3 --firmware en_watchdog-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

As the part attaches, the log says how it is wired:

```text
co-simulation: part wdt (watchdog) attached: wired RESET=EN WDI=GPIO4; drives no pin; may hold the chip in reset through EN
```

Then the firmware runs, hangs, and is reset, over and over:

```text
WDT: boot reset_reason=1 (POWERON)
WDT: kicked 10 times, now hung
[INFO  periph] EN low: the chip is held in reset
[INFO  periph] EN released: the chip restarts as from power-on
WDT: boot reset_reason=1 (POWERON)
```

The two `[INFO  periph]` lines are the emulator's log; leave `RUST_LOG=info`
out and only the firmware's lines show. About a second passes on the chip's clock between
the last kick and EN falling, the supervisor's window, and 200 ms between EN
falling and the boot.

## Drive it

In a second terminal, connect to the emulator's control channel:

```sh
nc 127.0.0.1 5556
```

| Type | What happens |
|---|---|
| `part wdt state` | `ok cause=late resets=1 resetting=false`: what the supervisor shows |
| `part wdt input reset` | the supervisor's manual reset: one pulse now |
| `en low` | the host holds EN low too: the chip stays in reset, and the supervisor's count stands still |
| `en high` | the host lets go, and the chip boots |

Add `--cosim-log events.jsonl --cosim-log-filter EnDrive,EnLevel,ChipReset`
to the emulator's command to record the pulse with its times:

```json
{"dir":"from_part","msg":{"EnDrive":{"level":false}},"part":"wdt","t_ns":3038149375}
{"dir":"to_part","msg":{"EnLevel":{"level":false}},"part":"*","t_ns":3038150937}
{"dir":"from_part","msg":{"EnDrive":{"level":true}},"part":"wdt","t_ns":3238149375}
{"dir":"to_part","msg":{"EnLevel":{"level":true}},"part":"*","t_ns":3238149375}
{"dir":"to_part","msg":{"ChipReset":{"pin_levels":4194303}},"part":"*","t_ns":0}
```

## Build it from source

The part, as [its README](../../parts/watchdog/README.md) says, then the
firmware with ESP-IDF:

```sh
cd test_apps/en_watchdog
idf.py set-target esp32c3 && idf.py build && idf.py merge-bin -o merged_flash.bin
```

Put `watchdog.wasm` beside `board.json` and pass `build/merged_flash.bin` as
`--firmware`.
