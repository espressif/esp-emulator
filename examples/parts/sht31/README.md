# SHT31: an example I2C part

A virtual Sensirion SHT31-D temperature and humidity sensor, written in Rust.
The firmware talks to it over I2C with the stock `esp_driver_i2c`, as it would
to the real sensor. It is the part the
[sensor board](../../boards/sensor/README.md) runs. To start a part of your
own, copy the simpler [push button](../pushbutton/README.md) and follow its
"Make your own part" steps; this one shows what an I2C part adds: an I2C
claim, `ON_BUS` and `on_bus`.

## What it models

The sensor answers at 0x44, its address with the ADDR pin low, or at the
address the board gives it. The firmware writes a 16-bit command, most
significant byte first, then reads the answer. Every 16-bit word the sensor
sends is followed by its CRC: CRC-8, polynomial 0x31, initial value 0xFF.

| Command | What it does |
|---|---|
| `0x2400`, `0x240B`, `0x2416` | Single-shot measurement at high, medium and low repeatability. The next read gets six bytes: temperature MSB, LSB and CRC, then humidity MSB, LSB and CRC. |
| `0x2C06`, `0x2C0D`, `0x2C10` | The same, with clock stretching. |
| `0xF32D` | Read the status register: the next read gets two bytes and a CRC. |
| `0x3041` | Clear the status register's flags: bits 15, 11, 10 and 4. |
| `0x30A2` | Soft reset: the status register goes back to `0x8010`, its power-on value, and an unread answer is lost. |

The raw words convert as the datasheet says: T = -45 + 175 × raw / 65535 °C
and RH = 100 × raw / 65535 %RH.

A command and its read may come as two transactions or as one. A read takes
the answer, so a second read, or a read with nothing asked for, is NACKed, as
on the real sensor. A read the master's driver splits into pieces (the legacy
`driver/i2c.h` sends the last byte on its own) arrives as requests that
`continue` the first, and those carry on through the same answer. A read
longer than the answer gets 0xFF for the rest. A probe (`i2c_master_probe`) is
acknowledged, and so is a command the sensor does not know, which does nothing
but set the status register's bit 1, "command not processed", until the next
known command.

## What it does not model

- Time. A measurement is ready at once. The real sensor takes up to 15 ms, and
  without clock stretching it NACKs a read until then.
- Noise. The three repeatabilities read the same.
- Periodic mode (`0x20xx` to `0x23xx`, `0x27xx`, ART `0x2B32`), Fetch Data
  (`0xE000`) and Break (`0x3093`).
- The heater (`0x306D`, `0x3066`), the ALERT pin and its limits, and the
  nRESET pin.

## Interactions

| Name | What it does |
|---|---|
| `warmer`, `cooler` | Temperature up or down 1 °C. |
| `wetter`, `drier` | Humidity up or down 5 %RH. |
| `set:temperature=<°C>`, `set:humidity=<%RH>` | That value, in one step. The value is a plain decimal, such as `31.5` or `-10`. |

It starts at 22.5 °C and 45 %RH, and stays between -45 and 130 °C and between
0 and 100 %RH, the range a reading can carry.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/sht31.wasm`. `cargo test`
runs the sensor's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with `SCL` on GPIO 6 and `SDA` on
GPIO 5:

```json
{ "parts": { "sensor": { "module": "sht31.wasm", "pins": { "SCL": 6, "SDA": 5 } } } }
```

With the ADDR pin high the sensor is at 0x45: add `"addr": "0x45"` beside
`pins`. A sensor not wired to both `SCL` and `SDA` (the names are upper case)
is refused when the board loads, with the missing pin named.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The sensor (`Sht31`), as an `esp_part::Device` whose `on_bus` answers the firmware's transactions, and the `export_part!` line that makes it a part: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md), over the shared buffer. |
