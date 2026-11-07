# TMP123: an example SPI part

A virtual Texas Instruments TMP121/TMP123 temperature sensor, written in Rust.
The firmware reads it over SPI with the stock `esp_driver_spi`, as it would the
real sensor. It is the part the
[thermometer board](../../boards/thermometer/README.md) runs. To start a part
of your own, copy the simpler [push button](../pushbutton/README.md) and follow
its "Make your own part" steps; this one shows what an SPI part adds: an SPI
claim, `ON_BUS` and `on_bus`.

## What it models

The sensor has no data input. While its chip select is low it clocks out one
16-bit word, most significant bit first: bits 15 to 3 are the temperature in
13-bit two's complement at 0.0625 °C a count, bit 2 is 0, and bits 1 and 0 are
not driven, so they read 0 here. The datasheet's Table 2 (SBOS273C) is the
reference: 25 °C is `0x0C80`, -25 °C is `0xF380`.

A read gets the word, most significant byte first. A read longer than two bytes
gets 0xFF for the rest, a line nobody drives; a shorter one gets the start of
the word. A transfer that reads nothing is not answered: the sensor ignores
what is written to it.

The part claims the pad on its `CS` pin as its chip select, with `SCK` and `SO`
beside it for the emulator to check. Which SPI bus and chip-select slot that
pad is, the emulator works out from the firmware's pin setup.

## What it does not model

- Conversion time. The real sensor converts continuously, about every 240 ms,
  and a read during a conversion gets the last result. Here a read gets the
  temperature now.
- Accuracy and noise. A reading is the exact temperature, rounded to 0.0625 °C.
- The difference between the TMP121 and the TMP123, which is the package.

## Interactions

| Name | What it does |
|---|---|
| `warmer`, `cooler` | Temperature up or down 1 °C. |
| `set:temperature=<°C>` | That value, in one step. The value is a plain decimal, such as `31.5` or `-10`. |

It starts at 25 °C and stays between -55 and 150 °C, the range of the
datasheet's Table 2.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/tmp123.wasm`. `cargo test`
runs the sensor's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with `CS` on GPIO 7, `SCK` on GPIO 6
and `SO` on GPIO 2:

```json
{ "parts": { "thermometer": { "module": "tmp123.wasm", "pins": { "CS": 7, "SCK": 6, "SO": 2 } } } }
```

A sensor with no `CS` (the names are upper case) is refused when the board
loads. A clock or data pad that the firmware puts on a different SPI bus than
the chip select is reported once it does.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The sensor (`Tmp123`), as an `esp_part::Device` whose `on_bus` answers the firmware's reads, and the `export_part!` line that makes it a part: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md), over the shared buffer. |
