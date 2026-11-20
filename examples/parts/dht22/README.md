# DHT22: an example temperature and humidity sensor part

A virtual DHT22 (AM2302) temperature and humidity sensor, written in Rust.
Firmware reads it by timing the levels on one data line, as it would the real
sensor; no driver in the emulator knows the sensor is there. It is the part
the [DHT board](../../boards/dht/README.md) runs. To start a part of your own,
copy the simpler [push button](../pushbutton/README.md) and follow its "Make
your own part" steps; this one shows a part that answers inside a window the
firmware opened, with a whole waveform in one message.

## What it models

One data line with a pull-up. The host pulls it low for at least 1 ms and
lets go; the sensor answers:

| Level | Time |
|---|---|
| wait after the host lets go | 30 µs |
| low, then high (the presence pulse) | 80 µs each |
| each of 40 bits: low, then high | 50 µs, then 26 µs for a 0 or 70 µs for a 1 |
| a last low, then the line let go | 50 µs |

The 40 bits are the relative humidity and the temperature in tenths, 16 bits
each, most significant bit first (the temperature's top bit is its sign),
then a checksum: the low byte of the sum of the other four bytes. A reading
of 45.0 %RH and 23.5 °C is `01 C2 00 EB AE`.

The part watches the line as the chip drives it, which never includes what
the part itself drives, so its own reply is never taken for a request. A low
of at least 500 µs followed by a release is a request; the reply goes out as
one `PulseDrive` stamped 30 µs after the release
([PART_ABI.md](../../../proto/PART_ABI.md#playing-a-waveform)), so every
width lands within a peripheral tick. A part in a browser answers after the
emulator has moved on; its reply then starts at once with every width kept.

## What it does not model

- The 2-second minimum between reads, and the stale reading a real sensor
  returns when read sooner. Every request is answered with the current values.
- Self-heating, noise, and the DHT11's whole-number format.

## Interactions

| Name | What it does |
|---|---|
| `set:humidity=V` | The relative humidity, in percent, to a tenth: 0 to 99.9. |
| `set:temperature=V` | The temperature, in degrees Celsius, to a tenth: -40 to 80. |

It shows `humidity` and `temperature` as the next reply carries them, and
`reads`, the replies handed over since it started. It starts at 45.0 %RH
and 23.5 °C.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/dht22.wasm`. `cargo test`
runs the part's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with `DATA` on GPIO 5:

```json
{ "parts": { "dht": { "module": "dht22.wasm", "pins": { "DATA": 5 } } } }
```

Without `DATA` the sensor is not wired and answers nothing.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The sensor (`Dht22`), as an `esp_part::Device` whose `on_pins` notices the host's start pulse and whose `poll` hands the reply over with `play`, and the `export_part!` line that makes it a part: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md), over the shared buffer. |
