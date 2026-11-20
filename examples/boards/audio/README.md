# Audio board: a microphone and an amplifier on I2S

ESP32-C3 firmware that reads an INMP441 microphone and plays what it hears to
a MAX98357A amplifier, run by `esp-emu` with no hardware and no browser. Both
are parts compiled to WebAssembly,
[`examples/parts/inmp441`](../../parts/inmp441) and
[`examples/parts/max98357a`](../../parts/max98357a). `board.json` wires the
microphone's `SD` to GPIO 7, where the chip receives, and the amplifier's
`DIN` to GPIO 6, where it transmits. The firmware uses the stock `i2s_std`
driver on I2S0, both directions at 16 kHz in 32-bit stereo slots; it does not
know either part is virtual.

The microphone hears a tone, 440 Hz at -12 dBFS until you change it. The
emulator puts no bit clock on a pad and a part needs none: each part is told
the rate and layout of its direction, and the microphone keeps its samples
50 ms ahead of emulated time.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`audio/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: the microphone's `SD` on GPIO 7, the amplifier's `DIN` on GPIO 6 |
| `inmp441.wasm`, `max98357a.wasm` | the parts |
| `i2s_echo-esp32c3.bin` | the firmware: [`test_apps/i2s_echo`](../../../test_apps/i2s_echo) for esp32c3 |

In that folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware i2s_echo-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

Once a second the firmware prints what it read in the left slot, where the
microphone puts its samples: the pitch, the peak, and how much of the second
was silence:

```text
I2S_ECHO: tone=440 Hz peak=-12 dBFS silent=0 ms
```

In a second terminal, connect to the emulator's control channel:

```sh
nc 127.0.0.1 5556
```

and type these lines, each followed by Enter:

```text
part mic input set:tone-hz=880
part amp state
```

The first answers `ok`, and from the next second the firmware reads
`tone=880 Hz`. The second answers with what reaches the amplifier's speaker:
`ok channel=mix frequency-hz=880 level-dbfs=-18 playing=true`. The level is
6 dB below the microphone's because the amplifier plays the mix of both
channels, and the firmware echoes the microphone's silent right slot too.
Ctrl-C closes `nc`; the emulator keeps running.

## What you can send

One command a line, one answer a line:

| Command | Answer |
|---|---|
| `part list` | `ok mic amp` |
| `part mic input higher`, `lower` | `ok`; the tone an octave up or down |
| `part mic input louder`, `quieter` | `ok`; 6 dB up or down |
| `part mic input set:tone-hz=1000`, `set:level-dbfs=-20` | `ok`; that value, in one step |
| `part mic input tone`, `feed` | `ok`; back to the tone, held while a file feeds it, or back to what is fed |
| `part mic state` | `ok level-dbfs=-12 rate-hz=16000 slot=left source=tone tone-hz=440` |
| `part amp state` | `ok channel=mix frequency-hz=440 level-dbfs=-18 playing=true` |
| `part clock` | `ok MS`: emulated time |

Anything else answers `err: ...`. `--cosim-log events.jsonl` records what
crosses between the chip and the parts, one JSON line a message: each
direction's format as `I2sFormat`. The samples themselves, `I2sOut` and
`I2sIn`, are left out unless `--cosim-log-filter` names them, as there is one
of each per DMA buffer.

## Hear it, and feed it

`--part-wav amp=FILE` records what reaches the amplifier's speaker to a WAV
file, mono 16-bit at the rate the firmware plays at; the file is complete
once the emulator exits. `--part-wav-in mic=FILE` makes the microphone hear
a WAV file in place of its tone: mono 16-bit at the rate the firmware
records at, 16 kHz here, or it is refused when the firmware starts its
receiver:

```sh
/path/to/esp-emu --chip esp32c3 --firmware i2s_echo-esp32c3.bin \
    --cosim-board board.json --part-wav-in mic=speech.wav --part-wav amp=heard.wav
```

`part mic input tone` on the control channel brings the tone back, and
`part mic input feed` the file.

## Build the pieces yourself

- The parts: [`examples/parts/inmp441`](../../parts/inmp441) and
  [`examples/parts/max98357a`](../../parts/max98357a) say how; the builds
  give `inmp441.wasm` and `max98357a.wasm`.
- The firmware, with ESP-IDF: in `test_apps/i2s_echo`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put them next to `board.json`, the firmware as `i2s_echo-esp32c3.bin`, and
run as above. The firmware uses the same pads on every chip with I2S, so this
board runs it on any of them. To write a part of your own, see
[docs/guides/PARTS.md](../../../docs/guides/PARTS.md).
