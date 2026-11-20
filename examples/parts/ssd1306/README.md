# SSD1306: an example part that draws

A virtual 128 x 64 monochrome OLED module driven by a Solomon Systech
SSD1306, written in Rust. The firmware writes to it over I2C with the stock
`esp_driver_i2c`, as it would to the real module, and a page or a script
reads back what the screen shows. It is the part the
[OLED board](../../boards/oled/README.md) runs. To start a part of your own,
copy the simpler [push button](../pushbutton/README.md); this one shows what
a part that draws adds: `render`.

## What it models

The module answers at 0x3C, its address with SA0 low, or at the address the
board gives it (0x3D with SA0 high). Each transfer starts with a control byte:
with D/C# (bit 6) clear the bytes after it are commands, with it set they are
display data, and with Co (bit 7) set only the next byte is, and another
control byte follows it. Display data lands in GDDRAM, eight pages of 128
columns, each byte eight pixels down its page, bit 0 on top. It follows the
SSD1306 datasheet, revision 1.1:

| Command | What it does |
|---|---|
| `AEh`, `AFh` | Display off, on. The panel is dark while off, whatever GDDRAM holds. |
| `A6h`, `A7h` | Normal, inverse: a 0 in GDDRAM lit. |
| `A4h`, `A5h` | GDDRAM shown, every pixel lit. |
| `20h` | Horizontal, vertical or page addressing: where the next data byte goes. |
| `21h`, `22h` | The column and page windows of horizontal and vertical addressing; each puts the pointer at the window's start. |
| `00h`-`0Fh`, `10h`-`17h`, `B0h`-`B7h` | The column start and the page of page addressing. After column 127 the pointer goes back to the column start on the same page. |

It powers up in page addressing with the display off, as the controller does.
A command's parameters are taken as parameters, whichever transfer they come
in: contrast (`81h`), multiplex (`A8h`), display offset (`D3h`), clock
(`D5h`), pre-charge (`D9h`), COM pins (`DAh`), VCOMH (`DBh`), the charge pump
(`8Dh`) and the scroll setups (`26h`, `27h`, `29h`, `2Ah`, `A3h`) are read and
change nothing on the screen. A read gets zeros, acknowledged; a probe is
acknowledged.

## What it shows and draws

`render` hands out the screen as the panel shows it, when it changed since the
last call: 1024 bytes, a bit a pixel, rows top to bottom, 16 bytes a row, the
most significant bit leftmost, 1 lit. Pixel (x, y) is bit `7 - x % 8` of byte
`y * 16 + x / 8`. Asked again with nothing changed, it hands out nothing, and
whoever draws keeps what it drew.

`state` shows `on` and `inverted`, and the layout `render` uses: `width=128`,
`height=64`, `format=mono`.

## What it does not model

- Orientation. The segment remap (`A0h`, `A1h`), the COM scan direction
  (`C0h`, `C8h`), the start line (`40h`-`7Fh`) and the display offset are
  accepted and not applied: GDDRAM column 0 is the left of the screen and page
  0 its top, whatever they say. On a real module they make up for how the
  glass is bonded to the controller, which differs between modules, and a
  driver sends whichever its module needs to come out upright.
- Scrolling (`2Eh`, `2Fh`), contrast, and the panel's timing.
- The SPI and parallel interfaces.

It has no interactions: the firmware is what changes it.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/ssd1306.wasm`. `cargo test`
runs the part's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, with `SCL` on GPIO 6 and `SDA` on
GPIO 5:

```json
{ "parts": { "oled": { "module": "ssd1306.wasm", "pins": { "SCL": 6, "SDA": 5 } } } }
```

With SA0 high the module is at 0x3D: add `"addr": "0x3D"` beside `pins`. Then
`part oled render` on `--control-tcp` answers the screen in hex, and
`part oled state` what it is set to.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The module (`Ssd1306`), as an `esp_part::Device` whose `on_bus` decodes the firmware's transfers and whose `render` hands out the screen, and the `export_part!` line that makes it a part: every `part_*` function of [`proto/PART_ABI.md`](../../../proto/PART_ABI.md), over the shared buffer. |
