# MAX98357A: an example amplifier part

A virtual Maxim MAX98357A I2S class-D amplifier, written in Rust. The firmware
plays to it with the stock `i2s_std` driver, as it would to the real
amplifier. It is the amplifier the [audio board](../../boards/audio/README.md)
runs. To start a part of your own, copy the simpler
[push button](../pushbutton/README.md) and follow its "Make your own part"
steps; this one shows what a part that listens adds: an I2S claim, `ON_MESSAGE`,
`on_i2s_format` and `on_i2s_out`.

## What it models

The "MAX98357A/MAX98357B" datasheet (Maxim) is the reference. The amplifier
listens on the chip's I2S data output, in slots of 16, 24 or 32 bits, and
plays one channel, picked by the voltage on its `SD_MODE` pin:

| `SD_MODE` | What it plays |
|---|---|
| below 0.16 V | nothing: shut down |
| 0.16 to 0.77 V | the mix of both channels, (L+R)/2 |
| 0.77 to 1.4 V | the right channel |
| above 1.4 V | the left channel |

Wired to a chip pin, `SD_MODE` is a logic level: high plays the left channel
and low shuts the amplifier down. The right channel needs a voltage between
the two, which no logic level is. Left unwired, it plays the mix, as a
breakout board does with its own resistors on `SD_MODE`.

What reaches its speaker it hands to its host as mono 16-bit samples at the
transmit rate (`part_audio_out`), for a page to play, or for
`--part-wav amp=FILE` to record on the command line. It
keeps 250 ms of it for the host to take and drops the oldest past that. It also shows what reaches its speaker,
over the last 100 ms: whether it is playing, the pitch from the upward zero
crossings, and the peak level in dBFS. Samples that stop coming for 200 ms of
emulated time are silence.

The part claims the chip's data output on its `DIN` pin. Which I2S unit that
is, the emulator works out from the firmware's pin setup, and it tells the
part the transmit format before any samples.

## What it does not model

- The `GAIN` pin: the level it shows is the level of the data, before the
  amplifier's 3 to 15 dB.
- The bit clock and the frame clock: it needs neither on a pad, since the
  format message says the rate and the layout.
- The other formats the part accepts: left-justified and TDM. It reads what
  the format message says, which is how the emulator's I2S unit sends it.

## State

| Name | What it shows |
|---|---|
| `playing` | `true` while samples above zero reach the speaker |
| `frequency-hz` | the pitch of the last 100 ms, 0 when there is none |
| `level-dbfs` | the peak of the last 100 ms, -99 for silence |
| `channel` | `left`, `mix` or `shutdown` |

It takes no interactions.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/max98357a.wasm`.
`cargo test` runs the amplifier's own tests on your computer, without the
emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with the amplifier's `DIN` on GPIO 6,
the pad the firmware's I2S transmits on:

```json
{ "parts": { "amp": { "module": "max98357a.wasm", "pins": { "DIN": 6 } } } }
```

An amplifier with no `DIN` is refused when the board loads, and one on a pad
the firmware never puts an I2S data output on is named when the run ends.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The amplifier (`Max98357a`), as an `esp_part::Device` whose `on_i2s_out` takes what the chip plays, whose `audio_out` hands it to its host and whose `state` shows what reaches the speaker, and the `export_part!` line that makes it a part: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md), over an 8 KiB shared buffer, `part_audio_out` among them (`sounds`). |
