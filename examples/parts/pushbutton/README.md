# Push button: an example part

A complete virtual part, written in Rust: a push button wired between a GPIO
and ground. It is the part the button demo in
[docs/guides/PARTS.md](../../../docs/guides/PARTS.md) runs, and a starting
point for a part of your own.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/pushbutton.wasm`. Nothing
else needs installing: the [`esp-part`](../../../esp-part/README.md) crate it
depends on compiles the contract with protox, a `.proto` compiler written in
Rust.

`cargo test` runs the button's own tests on your computer, without the
emulator. That is also the place to look inside a part: it has no console, so
`println!` in a part prints nowhere.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The button (`PushButton`), as an `esp_part::Device`, and the `export_part!` line that makes it a part: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md), over the shared buffer. |

## Make your own part

The guide's [Make your own part](../../../docs/guides/PARTS.md#make-your-own-part)
walks through this with more context. In short:

1. **Copy this directory**, without its `target/` folder. If the copy lives
   outside this repository, it also needs `esp-part` and
   `proto/esp_cosim.proto`: copy the `esp-part` folder beside it, point
   `esp-part = { path = ... }` in `Cargo.toml` at it, and set
   `ESP_COSIM_PROTO_DIR` to the folder holding your copy of the `.proto` when
   you build.
2. **Name it** in `Cargo.toml`. The build then produces
   `target/wasm32-unknown-unknown/release/<name>.wasm`, with any `-` in the
   name turned into `_`.
3. **Describe your device** in `src/lib.rs` by changing `PushButton`: its
   `NAME`, its pin names (`SIGNAL`), the interactions it understands
   (`on_input`), the pins it drives (`claims` and `poll`) and what it shows
   (`state`). The `export_part!` line stays.
   [`esp-part`'s README](../../../esp-part/README.md) lists every method of
   `Device` and which parts need it. Two things to know: a pin no part drives
   reads high, so a device that rests low drives its resting level from its
   first `poll`; and `on_input` is not told the time, so a device that acts
   some time after an interaction notes it there and starts the clock in the
   next `poll`. The motion sensor in
   [`proto/PART_ABI.md`](../../../proto/PART_ABI.md#two-ways-to-act-at-a-time-and-when-to-use-each)
   does both.
4. **Build it** as above, then run it in the command-line emulator with a
   board file of your own, as [`examples/boards/button`](../../boards/button)
   does for this one, or in a page of your own: the guide's
   [Use parts in your own page](../../../docs/guides/PARTS.md#use-parts-in-your-own-page)
   has one to copy. The demo page runs only the push button.
