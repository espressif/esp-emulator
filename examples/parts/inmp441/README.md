# INMP441: an example microphone part

A virtual InvenSense INMP441 I2S MEMS microphone, written in Rust. The
firmware reads it with the stock `i2s_std` driver, as it would the real
microphone. It is the microphone the [audio board](../../boards/audio/README.md)
runs. To start a part of your own, copy the simpler
[push button](../pushbutton/README.md) and follow its "Make your own part"
steps; this one shows what a part that supplies samples adds: an I2S claim,
`ON_MESSAGE`, `on_i2s_format`, and `samples` from `poll`, paced in emulated time.

## What it models

The "INMP441 Omnidirectional Microphone with Bottom Port and I2S Digital
Output" datasheet (InvenSense) is the reference. The microphone drives the
chip's I2S data input. In each frame it sends one 24-bit two's complement
sample in the half its `L/R` pin selects, low the left and high the right,
and leaves the other half undriven, which the receiver reads as zeros. The
real part needs 64 bit clocks a frame, which is 32-bit slots; here the sample
goes into whatever slot width the firmware programmed, left-aligned, so a
16-bit receiver reads its top 16 bits.

What it hears is a sine tone, 440 Hz at a peak of -12 dBFS to start with. A
tone at or above half the sample rate is silence: the microphone's
decimation filter removes what the frame rate cannot carry.

Or it hears what its host feeds it: mono 16-bit samples at the receive rate
(`part_audio_in`), such as a person's own microphone, or a WAV file through
`--part-wav-in mic=FILE` on the command line. The first sound fed switches it
from its tone. The `tone` interaction switches it back and keeps it there,
dropping what it is fed, until the `feed` interaction. It keeps 250 ms of
what it is fed and drops the oldest past that; when the feed runs dry it
hears silence, as in a quiet room. Each sample is the top 16 bits of its
24-bit word.

The part claims the chip's data input on its `SD` pin. Which I2S unit that is,
the emulator works out from the firmware's pin setup. It waits until the
emulator tells it the receive format (the rate, slots and slot width the
firmware programmed), counts its stream from that moment, and keeps 50 ms of
samples queued ahead of emulated time, as
[`proto/PART_ABI.md`](../../../proto/PART_ABI.md) says a part that supplies
samples does. After a long jump in emulated time it starts again from now
rather than send samples for a time already past, and after a chip reset it
waits for the next format.

## What it does not model

- Acoustics: the sensitivity, the noise floor, the high-pass and low-pass
  corners of the frequency response, and the start-up time after power-on.
- The `CHIPEN` pin and the bit clock: it is always on, and it needs no clock
  on a pad, since the format message says the rate.
- The 7.8 to 50 kHz sample-rate range: it answers at any rate the firmware
  programs.

## Interactions

| Name | What it does |
|---|---|
| `higher`, `lower` | The tone an octave up or down. |
| `louder`, `quieter` | The tone 6 dB up or down. |
| `set:tone-hz=<hz>` | That pitch, in one step, rounded to a whole Hz. |
| `set:level-dbfs=<dB>` | That peak level, in one step, rounded to a whole dB. |
| `tone` | Back to the tone, kept while its host goes on feeding it. |
| `feed` | Back to what its host feeds it, silence until something is fed. |

The pitch stays between 60 Hz and 15 kHz, the datasheet's frequency response,
and the level between -90 and 0 dBFS. Its state shows `source` (`tone`, or
`fed` once its host has fed it a sound), `tone-hz`,
`level-dbfs`, `slot` (`left` or `right`) and `rate-hz`, the receive rate it
was told, 0 before it is told one.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/inmp441.wasm`. `cargo test`
runs the microphone's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with the microphone's `SD` on
GPIO 7, the pad the firmware's I2S receives on:

```json
{ "parts": { "mic": { "module": "inmp441.wasm", "pins": { "SD": 7 } } } }
```

`L/R`, if the board wires it to a chip pin, picks the right half while that
pin is high. A microphone with no `SD` is refused when the board loads, and
one on a pad the firmware never puts an I2S data input on is named when the
run ends.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The microphone (`Inmp441`), as an `esp_part::Device` whose `on_i2s_format` starts its stream, whose `audio_in` takes what its host feeds it and whose `poll` supplies the samples, and the `export_part!` line that makes it a part: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md), over a 16 KiB shared buffer, `part_audio_in` among them (`hears`). |
