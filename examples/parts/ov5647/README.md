# OV5647: an example MIPI CSI camera sensor part

A virtual OmniVision OV5647 camera sensor on the ESP32-P4's MIPI CSI link,
in a colour-bar test mode, written in Rust. The firmware probes it and
programs it over SCCB as `esp_cam_sensor`'s OV5647 driver does, and the
stock `esp_driver_cam` CSI controller receives its frames. It runs on the
[ESP32-P4 camera board](../../boards/camera-p4/README.md) beside the
[OV2640](../ov2640/README.md), which shows the same sensor side on the DVP
bus.

## What it models

SCCB is I2C at address `0x36` with 16-bit register addresses, most
significant byte first.

| Register | What the part does with it |
|---|---|
| `0x300A`, `0x300B` | The chip ID: `0x56`, `0x47` |
| `0x0100` bit 0 | Streaming; asleep while clear |
| `0x0103` bit 0 | Software reset: every register back to 0 |
| `0x3034` | The MIPI bit mode in its low nibble: `0x8` RAW8, `0xA` RAW10 |
| `0x3808`/`0x3809`, `0x380A`/`0x380B` | The output width and height: bits 11:8, then the low byte |
| `0x3036`, `0x380C`/`0x380D`, `0x380E`/`0x380F` | The frame rate: the pixel clock the PLL multiplier gives, over HTS × VTS |

Every other register keeps what the firmware wrote and reads it back. The
module is wired for two data lanes. While it streams, a size and a bit depth
are programmed, and the CSI host captures on two lanes, the part sends a GBRG
Bayer mosaic, `esp_cam_sensor`'s order for this sensor (a byte a sample at
RAW8, a little-endian 16-bit word at RAW10), a frame a period at the rate the
firmware programmed (15 a second until it programs the timing), in pieces of
up to its buffer: the last picture it was shown in that mode, else the eight
colour bars (white, yellow, cyan, green, magenta, red, blue, black). It asks
to be shown an RGB888 picture of the mode programmed (`part_video_format`) and
makes its mosaic of it; it also takes the mosaic itself, and refuses, saying
why, a picture of another size or one made for a mode the firmware has since
left. Its buffer holds an RGB888 picture of its largest mode, 1920x1080. A
host on another lane count decodes nothing, so the part holds its frames then.

## What it does not model

- A scene: what it sends is the picture it was shown, or the bars.
- The PLL itself. The pixel clock is `esp_cam_sensor`'s own reading of the
  multiplier, `0x3036` × 25 MHz over 32 at RAW8 or 30 at RAW10, which gives
  its 1920x1080 and 1280x960 modes their stated 30 and 45 frames a second.
  For its RAW8 modes the same reading gives other rates than the ones the
  driver states (53.6 rather than 50 at 800x800).
- The PWDN pin, mirroring, binning, exposure and the image-quality
  registers.

## Interactions

| Name | What it does |
|---|---|
| `set:fps=<n>` | The frame rate, from 1 to 60 a second, over the one the registers give. |

`part csi video-format` on the command-line emulator's control channel
answers what it wants (`ok 320 240 GBRG 15`), and `part csi video FILE` shows
it the picture in `FILE`, RGB888 of that size (`ok`, or `err:` and why not); a
page sends the worker `video_format` and `video_in`, and `--part-video
csi=SRC` streams pictures from files or a camera.

It shows `format` (the fourcc, or `none` before a size and a bit depth are
programmed), `width`, `height`, `fps`, `streaming`, `capturing`,
`frames`, the frames sent whole, and `shown`, the bytes of the last picture
it was shown, in its layout.

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/ov5647.wasm`. `cargo test`
runs the part's own tests on your computer, without the emulator.

## Put it on a board

A board file for `esp-emu --chip esp32p4 --cosim-board`, its SCCB on the pads
`test_apps/camera_part` uses:

```json
{ "parts": { "csi": { "module": "ov5647.wasm", "pins": { "SIOC": 16, "SIOD": 17 } } } }
```

The lanes are the chip's dedicated D-PHY pads, so only SCCB is wired; the
emulator refuses the sensor on a chip without a CSI host.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate: it builds to a `.wasm` module. It depends on [`esp-part`](../../../esp-part/README.md), the part SDK. |
| `src/lib.rs` | The sensor (`Ov5647`), as an `esp_part::Device` that answers SCCB, is told the CSI host's format in `on_csi_format` and sends its frames from `poll`, and the `export_part!` line that makes it a part. |
