# WS2812: an example LED strip part

A virtual WS2812 (NeoPixel) LED strip, written in Rust. Firmware drives it as
it would a real strip, here with the stock RMT transmit driver; the part
reads its data line and shows the colour of every pixel. It is the part the
[strip board](../../boards/ws2812/README.md) runs. To start a part of your
own, copy the simpler [push button](../pushbutton/README.md) and follow its
"Make your own part" steps; this one shows a part that reads the chip's
waveform on a pin, thousands of edges a frame, in runs.

## What it models

One data line, `DIN`. Each bit is a high then a low, about 1.25 µs in all:

| Bit | High | Low |
|---|---|---|
| 0 | about 0.4 µs | about 0.85 µs |
| 1 | about 0.8 µs | about 0.45 µs |

The part reads a high longer than 0.6 µs as a 1. Each pixel takes 24 bits,
green, red and blue, most significant bit first, and a low of 50 µs or more
latches the frame: every pixel shows its colour. However long the frame, the
part decodes as many whole pixels as arrived, up to 256.

The part claims `DIN` with `reads_edges`: the emulator hands it the line's
edges in runs (`GpioEdges`), each a first level and the widths after it,
rather than one message per edge
([PART_ABI.md](../../../proto/PART_ABI.md#part_on_messagelen-i32)). The latch
low is the absence of edges, so the part ends a frame on the first of a low
of 50 µs inside a run, a run that starts 50 µs or more after the line went
low, or a call whose time is 50 µs past the last edge. Runs come at most a
heartbeat after their edges, so a frame shows within a millisecond of its
end.

## What it does not model

- The data output (`DOUT`) to a next strip: one part is the whole chain.
- The SK6812's fourth (white) byte, and colour order other than GRB.
- Brightness, current and the reset time of newer parts (280 µs).

## Interactions

None: the strip only shows. It shows `frames`, the frames latched since it
started, `pixels`, how many the last frame had, and each pixel by its index,
`0`, `1` and so on, as `#rrggbb`.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/ws2812.wasm`. `cargo test`
runs the part's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with `DIN` on GPIO 8:

```json
{ "parts": { "strip": { "module": "ws2812.wasm", "pins": { "DIN": 8 } } } }
```

Without `DIN` the strip is not wired and shows nothing.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The strip (`Ws2812`), as an `esp_part::Device` whose `on_edges` decodes each run and whose `on_pins` and `poll` latch a frame once the line has been low long enough, and the `export_part!` line that makes it a part over a 4096-byte buffer: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md). |
