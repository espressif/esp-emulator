# ESP32-P4 camera board: a DVP sensor and a MIPI CSI sensor

ESP32-P4 firmware capturing frames from two camera sensors with the stock
`esp_driver_cam` controllers, run by `esp-emu` with no hardware and no
browser: an OV2640 on the DVP bus ([`examples/parts/ov2640`](../../parts/ov2640))
and an OV5647 on the MIPI CSI link ([`examples/parts/ov5647`](../../parts/ov5647)),
both compiled to WebAssembly. `board.json` wires the OV2640 as the
[camera board](../camera/README.md) does, and both sensors' SCCB pins to
GPIO 16 and 17, one I2C bus with the OV2640 at `0x30` and the OV5647 at
`0x36`. The CSI lanes are the chip's own pads, so nothing else is wired.
The firmware probes and programs each sensor over SCCB, as a sensor driver
does, then checks the colour bars each sends; it does not know the sensors
are virtual.

## Run it from a release

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit's
`camera-p4/` folder holds this board, ready to run:

| File | What it is |
|---|---|
| `board.json` | the board: an OV2640 on DVP and an OV5647 on CSI, both on one I2C bus |
| `ov2640.wasm`, `ov5647.wasm` | the parts |
| `camera_part-esp32p4.bin` | the firmware: [`test_apps/camera_part`](../../../test_apps/camera_part) for esp32p4 |

In that folder:

```sh
/path/to/esp-emu --chip esp32p4 --firmware camera_part-esp32p4.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware prints `OV2640: pid=26 ver=42` and three lines
`DVP frame N: size=153600 bars ok`, then waits in JPEG mode for a picture as
the [camera board](../camera/README.md) does (`part camera video FILE`; with
none shown in 10 s it prints `JPEG: no picture shown, skipped`), then `OV5647: id=5647` and three lines `CSI frame N: size=76800 bars ok`, and
`CAMERA_PART PASSED`. While it runs,
`nc 127.0.0.1 5556` and `part csi state` answers with the OV5647's format,
size, rate and frames sent: `format=GBRG height=240 width=320` among them.

## What you can send

| Command | Answer |
|---|---|
| `part list` | `ok camera csi` |
| `part camera state`, `part csi state` | a sensor's format, size, rate, and frames sent |
| `part csi input set:fps=30` | `ok`; the OV5647 runs at 30 frames a second |
| `part clock` | `ok MS`: emulated time |

## Build the pieces yourself

- The parts: [`examples/parts/ov2640`](../../parts/ov2640) and
  [`examples/parts/ov5647`](../../parts/ov5647) say how.
- The firmware, with ESP-IDF 6.0 or later: in `test_apps/camera_part`,
  `idf.py set-target esp32p4 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin`.

Put them next to `board.json`, the firmware as `camera_part-esp32p4.bin`,
and run as above.
