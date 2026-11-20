# OV2640: an example camera sensor part

A virtual OmniVision OV2640 camera sensor on the chip's DVP bus, in its
colour-bar test mode, written in Rust. The firmware probes it and programs it
over SCCB as `esp_cam_sensor`'s OV2640 driver does, and the stock
`esp_driver_cam` DVP controller receives its frames. It is the part the
[camera board](../../boards/camera/README.md) runs. To start a part of your
own, copy the simpler [push button](../pushbutton/README.md) and follow its
"Make your own part" steps; this one shows a part that sends frames larger
than its buffer, at a rate of its own.

## What it models

SCCB is I2C at address `0x30` with 8-bit registers in two banks: register
`0xFF` picks the DSP bank (0) or the sensor bank (1).

| Register | Bank | What the part does with it |
|---|---|---|
| `0x0A`, `0x0B`, `0x1C`, `0x1D` | sensor | The chip ID: `0x26`, `0x42`, `0x7F`, `0xA2` |
| `0x12` (COM7) bit 7 | sensor | Soft reset: every register back to 0 |
| `0x5A` (ZMOW), `0x5B` (ZMOH), `0x5C` (ZMHH) | DSP | The output size in units of four pixels, ZMHH holding bits 9:8 of the width's and bit 8 of the height's |
| `0xDA` (IMAGE_MODE) | DSP | The output format: YUV422 (`0x00` YUYV, `0x01` UYVY), RGB565 (`0x08` high byte first, `0x09` low byte first), `0x40` grey, `0x10` JPEG |

Every other register keeps what the firmware wrote and reads it back. Once a
size is programmed and the chip's receiver captures, the part sends a frame
at that size and in that format, 15 a second, each a period after the last:
the last picture it was shown in that mode, else the eight colour bars
(white, yellow, cyan, green, magenta, red, blue, black). It asks to be shown
a picture of the mode programmed (`part_video_format`): RGB888 of that size,
which it makes into the mode's layout, or in JPEG mode a JPEG file, which it
sends as it is, since it makes none. It refuses, saying why, a picture of
another size, one that is not a JPEG in JPEG mode, or one made for a mode the
firmware has since left. A picture comes in whole, so its buffer is 1 MiB:
RGB888 up to VGA, and any JPEG that size; it asks for nothing larger.

## What it does not model

- A scene: what it sends is the picture it was shown, or the bars.
- A JPEG it makes itself, and RAW10: in those modes it sends only what it
  was shown in them, and in RAW10 nothing.
- The rate the clock and timing registers set: the part runs at 15 frames a
  second, or what `set:fps=` asks.
- The PWDN and RESET pins, mirroring, and the image-quality registers.

## Interactions

| Name | What it does |
|---|---|
| `set:fps=<n>` | The frame rate, from 1 to 60 a second. |

It shows `format` (the fourcc, or `none` before a size is programmed),
`width`, `height`, `fps`, `capturing`, `frames`, the frames sent whole, and
`shown`, the bytes of the last picture it was shown.

## Show it a picture

On the command-line emulator's control channel, `part camera video-format`
answers what it wants (`ok 320 240 JPEG 15`), and `part camera video FILE`
shows it the picture in `FILE`, RGB888 of that size or a JPEG (`ok`, or `err:`
and why not). `--part-video camera=SRC` streams pictures to it from a JPEG
file, a folder of them, an MJPEG file or an MJPEG stream on stdin. In a page, the worker's
`video_format` and `video_in` messages do the same; the browser build's
`camera-demo.html` shows it a test card in JPEG mode and, on a click, the webcam.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/ov2640.wasm`. `cargo test`
runs the part's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --cosim-board`, on the pads `test_apps/camera_part`
uses:

```json
{ "parts": { "camera": { "module": "ov2640.wasm", "pins": {
    "D0": 4, "D1": 5, "D2": 6, "D3": 7, "D4": 8, "D5": 9, "D6": 10, "D7": 11,
    "PCLK": 12, "VSYNC": 13, "HREF": 14, "XCLK": 15, "SIOC": 16, "SIOD": 17 } } } }
```

`D0` to `D7` are the sensor's 8-bit bus (its Y2 to Y9). Without `PCLK`,
`VSYNC` and all eight data pins the part is not on a camera bus; without
`SIOC` and `SIOD` the firmware cannot reach it.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The sensor (`Ov2640`), as an `esp_part::Device` that answers SCCB, is told the receiver's format in `on_dvp_format` and sends its frames from `poll`, and the `export_part!` line that makes it a part. |
