# 93C46: an SPI part on a chip select the firmware drives itself

A virtual Atmel AT93C46D three-wire serial EEPROM, written in Rust. It is the
EEPROM of ESP-IDF's `examples/peripherals/spi_master/hd_eeprom`, which runs
against it unchanged: the stock `esp_driver_spi` clocks the instructions, and
the example's own driver works the chip select by hand. It is the part the
[EEPROM board](../../boards/eeprom/README.md) runs. To start a part of your
own, copy the simpler [push button](../pushbutton/README.md) and follow its
"Make your own part" steps; the [TMP123](../tmp123/README.md) shows what an
SPI part adds, and this one shows two more things: a chip select that is a
plain GPIO, and a pin the part drives back.

## What it models

128 bytes (the 8-bit organisation, ORG low) behind a 10-bit instruction: a
start bit, a two-bit opcode and a seven-bit address, which the firmware sends
as the transfer's command phase (`command_bits = 10`). READ answers the byte
at the address and the ones after it for as long as the master clocks; WRITE
and ERASE change a byte; EWEN and EWDS enable and disable writing, and a
write or erase while disabled is ignored, as on the chip; ERAL and WRAL erase
or write the whole array. A fresh chip is erased, every byte 0xFF.

The chip is selected by a chip select that is **high**. After a write or
erase the driver drops the chip select and raises it again to read the busy
signal on DO: the part drives DO low from the drop, and high again 2 ms after
the rise, inside the datasheet's 5 ms tWC. The example's `ready_rising_isr`
sees that rising edge, or its polling variant reads the level.

## How it is routed

The example sets `spics_io_num = -1` and drives the chip select as a GPIO,
because the busy signal must be read with the chip select high and nothing
clocking, which no SPI transaction does. So the pad carries no chip select of
the controller's, and the emulator cannot tell from it which bus the device is
on. The part's claim therefore names the bus pads (`SK`, `DI`, `DO`) beside
the chip select, and says the device selects when high (`cs_active_high`).
The emulator takes the bus from whichever of those pads the firmware put a
line of a bus on, and routes each transfer to the part while the chip-select
pad is high as the transfer is clocked. A transfer while it is low reaches no
part, and a read gets 0xFF.

## What it does not model

- The 16-bit organisation (ORG high). The example uses 8-bit.
- The dummy zero bit the chip clocks out before READ data. The example
  absorbs it with `input_delay_ns`; here the data byte is the answer.
- The sequential-read timing and the power-up state of the write enable
  (disabled here, as on the chip).

## Interactions

| Name | What it does |
|---|---|
| `erase` | Every byte 0xFF, as a fresh chip. |

`part eeprom state` shows `write-enabled` and `busy`.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/eeprom93c46.wasm`.
`cargo test` runs the chip's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, wired as the example wires it on an
ESP32-C3: `CS` on GPIO 10, `SK` on 11, `DI` on 12, `DO` on 13:

```json
{ "parts": { "eeprom": { "module": "eeprom93c46.wasm", "pins": { "CS": 10, "SK": 11, "DI": 12, "DO": 13 } } } }
```

A chip with no `CS` (the names are upper case) is refused when the board
loads. One whose `SK`, `DI` and `DO` the firmware never put a bus line on is
named at the end of the run: with the chip select a plain GPIO, those pads are
the only way to tell its bus.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The chip (`Eeprom93c46`), as an `esp_part::Device`: `on_bus` decodes the instruction and answers reads, `on_pins` follows the chip select, `poll` drives the busy signal on DO; and the `export_part!` line that makes it a part. |
