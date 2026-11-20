# Camera board: an OV2640 on the DVP bus

ESP32-S3 firmware capturing frames from an OV2640 camera sensor with the
stock `esp_driver_cam` DVP controller, run by `esp-emu` with no hardware and
no browser. The sensor is [`examples/parts/ov2640`](../../parts/ov2640)
compiled to WebAssembly; `board.json` wires its 8-bit bus to GPIO 4 to 11,
`PCLK`, `VSYNC`, `HREF` and `XCLK` to GPIO 12 to 15, and its SCCB pins to
GPIO 16 and 17. The firmware probes the sensor and programs its output over
SCCB, as a sensor driver does, then checks the colour bars it receives; it
does not know the sensor is virtual.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`camera/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: one OV2640 on the DVP bus and on I2C |
| `ov2640.wasm` | the part |
| `camera_part-esp32s3.bin` | the firmware: [`test_apps/camera_part`](../../../test_apps/camera_part) for esp32s3 |

In that folder:

```sh
/path/to/esp-emu --chip esp32s3 --firmware camera_part-esp32s3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware prints `OV2640: pid=26 ver=42` once it has probed the sensor,
then a line per frame, `DVP frame 0: size=153600 bars ok`, three of them.
Then it puts the sensor in JPEG mode and prints `OV2640: jpeg ok`: a sensor
makes JPEG from its scene, and this one has none, so it sends only a JPEG it
is shown. With none shown in 10 seconds it prints `JPEG: no picture shown,
skipped` and `CAMERA_PART PASSED`. To show it one, in a second terminal:

```sh
nc 127.0.0.1 5556
```

and type, each followed by Enter:

```text
part camera video-format
part camera video /path/to/picture.jpg
```

The first answers `ok 320 240 JPEG 15`, what the sensor asks to be shown;
the second shows it the JPEG file and answers `ok`. The firmware prints
`JPEG frame 0: size=N crc=C` for three frames of it, the size and CRC32 of
your file, then `CAMERA_PART PASSED`. `part camera state` answers what the
firmware programmed, the frames sent so far and the size of the picture
shown.

## What you can send

| Command | Answer |
|---|---|
| `part list` | `ok camera` |
| `part camera state` | the sensor's format, size, rate, frames sent and the picture shown |
| `part camera video-format` | `ok 320 240 JPEG 15`: what the sensor asks to be shown, or `ok` for nothing |
| `part camera video FILE` | `ok`, or `err:` and why not; shows the sensor the picture in FILE: a JPEG in JPEG mode, else RGB888 of the size asked |
| `part camera input set:fps=30` | `ok`; the sensor runs at 30 frames a second |
| `part clock` | `ok MS`: emulated time |

`--cosim-log events.jsonl` records every message between the chip and the
part, one JSON line each; the frames themselves are left out unless
`--cosim-log-filter` names `CamFrame`.

## Build the pieces yourself

- The part: [`examples/parts/ov2640`](../../parts/ov2640) says how; the build
  gives `ov2640.wasm`.
- The firmware, with ESP-IDF 6.0 or later: in `test_apps/camera_part`,
  `idf.py set-target esp32s3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put both next to `board.json`, the firmware as `camera_part-esp32s3.bin`, and
run as above. On ESP32-P4 the same firmware also captures from a MIPI CSI
sensor: the [ESP32-P4 camera board](../camera-p4/README.md) has both.
