# KY-040: an example rotary encoder part

A virtual KY-040 rotary encoder module, written in Rust. The firmware counts
it with the chip's pulse counter (PCNT) through the stock `esp_driver_pcnt`,
as it would the real knob. It is the part the
[encoder board](../../boards/encoder/README.md) runs. To start a part of your
own, copy the simpler [push button](../pushbutton/README.md) and follow its
"Make your own part" steps; this one shows a part that puts a waveform on two
pins rather than a level on one.

## What it models

The module carries an EC11-type encoder: 20 detents a turn, two contacts
`CLK` (the encoder's A) and `DT` (its B) that make a quadrature pair, and a
push switch `SW`, all closing to ground. The lines rest high on pull-ups.

One detent moves the two lines through four states, one line at a time:
`CLK DT` = `11 -> 01 -> 00 -> 10 -> 11`. Turned clockwise, `CLK` moves first;
counter-clockwise, `DT` does, the waveform IDF's
`examples/peripherals/pcnt/rotary_encoder` README draws. With that example's
channel set-up, a clockwise detent counts +4 and a counter-clockwise one -4.

The part sends one edge each time the emulator asks it (`part_poll`, once a
millisecond of emulated time while the chip runs, less often while it
sleeps), stamped with that time and at least 250 µs after the one before. A
detent therefore takes four polls, 4 to 40 ms of emulated time, so the knob
turns at most 25 to 100 detents a second, depending on the chip and on how
much its firmware sleeps ([PARTS.md](../../../docs/guides/PARTS.md#time-inside-a-part)
has the figures). Turned faster, the turns wait in the part and play out in
order afterwards: the count comes out right, later. No edge is dropped. The
switch goes ahead of any edges still to come. A part may hand over several
edges in one poll, stamped ahead
([PARTS.md](../../../docs/guides/PARTS.md#time-inside-a-part)); this one
stamps each with the poll's time, so it keeps to one a heartbeat.

## What it does not model

- Contact bounce. The lines move cleanly, so a glitch filter has nothing to
  remove.
- Half-step and 30-detent variants, which rest with the lines low every other
  detent.

## Interactions

| Name | What it does |
|---|---|
| `cw`, `ccw` | Turn one detent clockwise or counter-clockwise. |
| `press`, `release` | Close or open the switch. |

It shows `position`, the detents turned since it started, clockwise
positive, and `pressed`.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/ky040.wasm`. `cargo test`
runs the encoder's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with `CLK` on GPIO 5, `DT` on GPIO
6 and `SW` on GPIO 7:

```json
{ "parts": { "encoder": { "module": "ky040.wasm", "pins": { "CLK": 5, "DT": 6, "SW": 7 } } } }
```

A line the board leaves out is not driven; without `CLK` and `DT` the knob
turns nothing.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The encoder (`Ky040`), as an `esp_part::Device` whose `poll` sends the next edge, and the `export_part!` line that makes it a part: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md), over the shared buffer. |
