# VS1838B: an example infrared receiver part

A virtual VS1838B infrared receiver module and the remote control that talks
to it, written in Rust. The firmware times the module's output with the
chip's RMT receiver through the stock `esp_driver_rmt` and decodes NEC
frames from it, as it would with the real module. It is the part the
[IR board](../../boards/ir/README.md) runs. To start a part of your own, copy
the simpler [push button](../pushbutton/README.md) and follow its "Make your
own part" steps; this one shows a part that hands over a whole timed
waveform at once.

## What it models

The module turns a 38 kHz infrared carrier into a level on its `OUT` pin: high
while no carrier arrives, low while a burst lasts. The remote sends NEC
frames, the protocol IDF's `examples/peripherals/rmt/ir_nec_transceiver`
decodes:

| Mark | Burst (`OUT` low) | Space (`OUT` high) |
|---|---|---|
| leader | 9 ms | 4.5 ms |
| a 0 bit | 560 µs | 560 µs |
| a 1 bit | 560 µs | 1690 µs |
| end | 560 µs | until the next frame |

32 bits follow the leader, least significant first: the address (`0x00`),
its inverse, the command, its inverse. One frame is 68 edges over about
68 ms, and frames start at least 108 ms apart, as a remote holds them.

A key press plays one frame. The part stamps the frame's 68 edges ahead and
hands them over in one poll ([PARTS.md](../../../docs/guides/PARTS.md#time-inside-a-part)),
so each lands within a peripheral tick of its time, a microsecond or two,
well inside the 200 µs a NEC decoder allows. The first edge is 50 ms after
the press, which
leaves a browser page time to pass the frame on before it starts. Keys
pressed faster than a frame each wait in the part and play in order.

## What it does not model

- The carrier. `OUT` carries the demodulated level; nothing on the chip sees
  38 kHz.
- Repeat codes, the short frames a held key sends after its first. Each press
  is one full frame.
- Noise, a weak signal, and the module's own delay after a burst starts.

## Interactions

| Name | What it does |
|---|---|
| `0` to `9` | Press a digit key: commands `0x19`, `0x45`, `0x46`, `0x47`, `0x44`, `0x40`, `0x43`, `0x07`, `0x15`, `0x09`. |
| `star`, `hash` | Commands `0x16` and `0x0D`. |
| `up`, `down`, `left`, `right`, `ok` | Commands `0x18`, `0x52`, `0x08`, `0x5A` and `0x1C`. |

It shows `address` (always 0), and for the last frame that played out
`command` (decimal) and `key`, and `frames`, the frames played out since it
started. A frame a chip reset cut short is not counted.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/vs1838b.wasm`. `cargo test`
runs the part's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with `OUT` on GPIO 4:

```json
{ "parts": { "ir": { "module": "vs1838b.wasm", "pins": { "OUT": 4 } } } }
```

Without `OUT` the module is not wired and a key press sends nothing.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The module and its remote (`Vs1838b`), as an `esp_part::Device` whose `poll` hands over a pressed key's frame, and the `export_part!` line that makes it a part: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md), over the shared buffer. |
