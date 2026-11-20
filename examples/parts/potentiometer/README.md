# Potentiometer: an example analog part

A virtual potentiometer, written in Rust. The firmware reads its wiper with
the stock `adc_oneshot` driver, as it would a real one. It is the part the
[analog board](../../boards/analog/README.md) runs. To start a part of your
own, copy the simpler [push button](../pushbutton/README.md) and follow its
"Make your own part" steps; this one shows what an analog part adds: an
analog claim and `hold`.

## What it models

A linear potentiometer across a 3.3 V rail, its wiper on an ADC pad. The
wiper's voltage is the knob's position times the rail: 0 mV at 0 %, 3300 mV at
100 %. The part claims the pad (`analog(pin)`) and, from `poll`, holds the
wiper's voltage there (`hold(pin, mv, now)`) whenever the knob moves. The
emulator keeps that voltage on the pad until the next one, across a chip
reset; the firmware's conversion reads it at whatever attenuation the firmware
chose.

## What it does not model

- The track's resistance and the load the ADC puts on the wiper: the voltage
  is the ideal divider's.
- Noise, wear and the dead bands at the ends of a real track.

## Interactions

| Name | What it does |
|---|---|
| `more`, `less` | Turn the knob 10 % up or down. |
| `set:position=<percent>` | That position, 0 to 100, in one step. |

It starts half way, 1650 mV.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/potentiometer.wasm`.
`cargo test` runs the part's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with the wiper on GPIO 3:

```json
{ "parts": { "knob": { "module": "potentiometer.wasm", "pins": { "W": 3 } } } }
```

The pin must be an ADC pad of the chip (its `adc_channel.h`): GPIO 3 is ADC1
channel 3 on the C3 and C6 and channel 2 on the C5, H2 and S3; the P4's pads
are GPIO 16-23 and 49-54, the S31's GPIO 42-57. A pad with no ADC channel is refused when the board
loads.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The knob (`Potentiometer`), as an `esp_part::Device` whose `poll` holds the wiper's voltage on its pad, and the `export_part!` line that makes it a part. |
