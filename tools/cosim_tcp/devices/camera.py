# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import os
import struct
import sys

from .. import wire
from ..device import Device

# name: (7-bit address, register address bytes, {register: value}). The
# identity registers each driver's probe reads.
SENSORS = {
    "ov2640": (0x30, 1, {0x0A: 0x26, 0x0B: 0x42, 0x1C: 0x7F, 0x1D: 0xA2}),
    "ov3640": (0x3C, 2, {0x300A: 0x36, 0x300B: 0x4C}),
    "ov5640": (0x3C, 2, {0x300A: 0x56, 0x300B: 0x40}),
}
LEAD = 2
BPP = {"RGBP": 2, "RGBR": 2, "UYVY": 2, "YUYV": 2, "YVYU": 2, "VYUY": 2, "GREY": 1}
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def num(text):
    return int(text, 0)


def jpeg_size(data):
    """Width and height from a JPEG's frame header; (0, 0) if none is found."""
    i = 2
    while i + 9 < len(data) and data[i] == 0xFF:
        marker, (length,) = data[i + 1], struct.unpack(">H", data[i + 2:i + 4])
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + length
    return 0, 0


def pixel(four, rgb, odd):
    r, g, b = rgb
    if four in ("RGBP", "RGBR"):
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        return struct.pack(">H" if four == "RGBP" else "<H", v)
    y = (66 * r + 129 * g + 25 * b + 128 >> 8) + 16
    if four == "GREY":
        return bytes([y])
    u = (-38 * r - 74 * g + 112 * b + 128 >> 8) + 128
    v = (112 * r - 94 * g - 18 * b + 128 >> 8) + 128
    c = (v if odd else u) if four in ("UYVY", "YUYV") else (u if odd else v)
    return bytes([c, y]) if four in ("UYVY", "VYUY") else bytes([y, c])


def bars(four, width, height, n):
    """Colour bars, shifted one bar per frame."""
    row = b"".join(pixel(four, BARS[(x * 8 // width + n) % 8], x & 1) for x in range(width))
    return row * height


class Camera(Device):
    """A DVP camera sensor: answers the SCCB probe and sends frames.

    It answers the sensor's SCCB (I2C) probe and sends frames to the chip's
    camera receiver.

        esp-emu --chip esp32s31 --firmware app.bin --cosim-tcp 127.0.0.1:5594 &
        python3 tools/cosim-tcp.py 5594 camera --pclk 54 --vsync 56 --de 57 \\
            --xclk 55 --data 46,47,48,49,50,51,52,53 --scl 1 --sda 0 \\
            --sensor ov2640 --fourcc YUYV --size 640x480 --fps 6

    The pads are the ones the firmware's `esp_cam_ctlr_dvp_pin_config_t` and
    its SCCB bus name. `--frames` is a JPEG file or a directory of them,
    sent in name order and repeated; without it the sensor sends moving
    colour bars in `--fourcc` at `--size`. What the firmware programs over
    SCCB is acknowledged and read back, not interpreted: the format and
    size sent are the ones given here, so give the ones the firmware asks
    its sensor for.
    """

    name = "camera"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--pclk", type=int, required=True, help="the pad the pixel clock is on")
        ap.add_argument("--vsync", type=int, required=True)
        ap.add_argument("--de", type=int, help="data enable (HREF)")
        ap.add_argument("--xclk", type=int, help="the pad the chip's reference clock is on, if any")
        ap.add_argument("--data", required=True, help="the eight data pads, D0 first, comma separated")
        ap.add_argument("--scl", type=int, help="SCCB clock pad; with --sda, answer the sensor's I2C probe")
        ap.add_argument("--sda", type=int)
        ap.add_argument("--sensor", choices=sorted(SENSORS), default="ov2640",
                        help="whose identity registers to answer with (default %(default)s)")
        ap.add_argument("--addr", type=num, help="SCCB address, when not the sensor's usual one")
        ap.add_argument("--reg", action="append", default=[], metavar="REG=VAL",
                        help="another register's value, e.g. 0x300A=0x36 (repeatable)")
        ap.add_argument("--frames", help="a JPEG file, or a directory of them")
        ap.add_argument("--fourcc", default="RGBP", choices=sorted(BPP),
                        help="the colour bars' format (default %(default)s)")
        ap.add_argument("--size", default="320x240", help="the colour bars' size (default %(default)s)")
        ap.add_argument("--fps", type=float, default=15.0,
                        help="frames per second of emulated time (default %(default)s)")

    def __init__(self, args):
        super().__init__(args)
        self.data_pins = [int(p) for p in args.data.split(",")]
        if len(self.data_pins) != 8:
            sys.exit("camera: --data names eight pads")
        addr, self.reg_bytes, regs = SENSORS[args.sensor]
        self.regs = dict(regs)
        self.addr = args.addr if args.addr is not None else addr
        for item in args.reg:
            reg, _, val = item.partition("=")
            self.regs[num(reg)] = num(val)
        self.sccb = args.scl is not None and args.sda is not None

        # Encoded once: a frame built while the emulator runs on arrives late.
        if args.frames:
            paths = [args.frames]
            if os.path.isdir(args.frames):
                paths = sorted(os.path.join(args.frames, n) for n in os.listdir(args.frames)
                               if n.lower().endswith((".jpg", ".jpeg")))
            jpegs = [open(p, "rb").read() for p in paths]
            if not jpegs:
                sys.exit(f"no JPEG in {args.frames}")
            self.frames = [wire.cam_frame_pieces(j, *jpeg_size(j), wire.fourcc("JPEG")) for j in jpegs]
        else:
            width, height = (int(v) for v in args.size.lower().split("x"))
            self.frames = [wire.cam_frame_pieces(bars(args.fourcc, width, height, n), width, height,
                                                 wire.fourcc(args.fourcc)) for n in range(8)]

        self.period = int(1_000_000_000 / args.fps)
        self.capturing = False
        self.session = 0        # the capture session the receiver last named
        self.next_at = 0        # emulated time the next frame is due
        self.sent = 0
        self.pointer = 0        # the register a read starts at
        self.hold = False       # a format arrived: frames follow the next frame

    def claims(self):
        a = self.args
        claims = [wire.dvp_claim(a.pclk, a.vsync, self.data_pins, de_pin=a.de, xclk_pin=a.xclk)]
        if self.sccb:
            claims.append(wire.i2c_claim(self.addr, a.scl, a.sda))
        return claims

    def i2c(self, t):
        if t.write:
            self.pointer = int.from_bytes(t.write[:self.reg_bytes], "big")
            for i, b in enumerate(t.write[self.reg_bytes:]):
                self.regs[self.pointer + i] = b
            if len(t.write) > self.reg_bytes:
                self.pointer += len(t.write) - self.reg_bytes
        out = bytes(self.regs.get(self.pointer + i, 0) for i in range(t.read_len))
        self.pointer += t.read_len
        return True, out

    def dvp_format(self, f):
        print(f"receiver: {'capturing' if f['capturing'] else 'stopped'}, XCLK {f['xclk_hz']} Hz", flush=True)
        if f["capturing"] and not self.capturing:
            self.next_at = self.bus.now + self.period
        self.capturing, self.session = f["capturing"], f["session"]
        self.hold = True

    def tick(self, now):
        if self.hold:                                # frames follow the next heartbeat
            self.hold = False
            return
        # Stay LEAD frames ahead: an emulator that is not held to wall-clock
        # time (--realtime) runs past a frame sent only when its time has come.
        while self.capturing and self.next_at <= now + LEAD * self.period:
            for msg in wire.cam_frame(self.frames[self.sent % len(self.frames)], due_ns=self.next_at,
                                      session=self.session):
                self.bus.send(msg)
            self.sent += 1
            self.next_at += self.period

    def closed(self):
        print(f"sent {self.sent} frames", flush=True)


# name: (7-bit address, {register: value}): the chip-ID registers each MIPI
# sensor's probe reads (esp_cam_sensor), a16v8.
CSI_SENSORS = {
    "sc2336": (0x30, {0x3107: 0xCB, 0x3108: 0x3A}),
    "ov2710": (0x36, {0x300A: 0x27, 0x300B: 0x10}),
    "ov5647": (0x36, {0x300A: 0x56, 0x300B: 0x47}),
    "sc202cs": (0x36, {0x3107: 0xEB, 0x3108: 0x52}),
}
# The Bayer orders, the top-left cell first, and V4L2's names at 8 and 10 bits.
BAYER = {"BGGR": ("BA81", "BG10"), "GBRG": ("GBRG", "GB10"), "GRBG": ("GRBG", "BA10"), "RGGB": ("RGGB", "RG10")}


def mosaic(order, depth, width, height, n):
    """Colour bars, shifted one bar per frame, as a Bayer mosaic: a byte a
    sample at 8 bits, a little-endian u16 at 10."""
    cell = [ord(c) for c in order]                       # 'R', 'G' or 'B' at each 2x2 site
    idx = {ord("R"): 0, ord("G"): 1, ord("B"): 2}
    out = bytearray()
    for y in range(height):
        for x in range(width):
            v = BARS[(x * 8 // width + n) % 8][idx[cell[(y & 1) * 2 + (x & 1)]]]
            if depth > 8:
                out += struct.pack("<H", v << 2 | v >> 6)
            else:
                out.append(v)
    return bytes(out)


class CsiCamera(Device):
    """A MIPI CSI camera sensor on ESP32-P4: answers the SCCB probe and
    sends Bayer frames.

        esp-emu --chip esp32p4 --firmware app.bin --cosim-tcp 127.0.0.1:5594 &
        python3 tools/cosim-tcp.py 5594 csi-camera --scl 8 --sda 7 \\
            --sensor ov5647 --size 800x640 --order BGGR --depth 8 --fps 50

    The SCCB pads are the firmware's camera I2C bus. The frames are moving
    colour bars as a Bayer mosaic in `--order` at `--depth` bits and
    `--size`; give the size, depth and order the firmware programs the ISP
    or the bridge for, since a frame of another shape is dropped. What the
    firmware writes over SCCB is acknowledged and read back, not
    interpreted.
    """

    name = "csi-camera"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--scl", type=int, help="SCCB clock pad; with --sda, answer the sensor's I2C probe")
        ap.add_argument("--sda", type=int)
        ap.add_argument("--sensor", choices=sorted(CSI_SENSORS), default="sc2336",
                        help="whose identity registers to answer with (default %(default)s)")
        ap.add_argument("--addr", type=num, help="SCCB address, when not the sensor's usual one")
        ap.add_argument("--size", default="320x240", help="the frames' size (default %(default)s)")
        ap.add_argument("--order", default="BGGR", choices=sorted(BAYER),
                        help="the mosaic's top-left cell first (default %(default)s)")
        ap.add_argument("--depth", type=int, choices=(8, 10), default=8, help="bits a sample (default 8)")
        ap.add_argument("--fps", type=float, default=30.0,
                        help="frames per second of emulated time (default %(default)s)")

    def __init__(self, args):
        super().__init__(args)
        addr, regs = CSI_SENSORS[args.sensor]
        self.addr = args.addr if args.addr is not None else addr
        self.regs = dict(regs)
        self.sccb = args.scl is not None and args.sda is not None
        width, height = (int(v) for v in args.size.lower().split("x"))
        four = wire.fourcc(BAYER[args.order][args.depth > 8])
        # Encoded once: a frame built while the emulator runs on arrives late.
        self.frames = [wire.cam_frame_pieces(mosaic(args.order, args.depth, width, height, n), width, height, four)
                       for n in range(8)]
        self.period = int(1_000_000_000 / args.fps)
        self.capturing = False
        self.session = 0
        self.next_at = 0
        self.sent = 0
        self.pointer = 0
        self.hold = False

    def claims(self):
        claims = [wire.csi_claim()]
        if self.sccb:
            claims.append(wire.i2c_claim(self.addr, self.args.scl, self.args.sda))
        return claims

    def i2c(self, t):
        if t.write:
            self.pointer = int.from_bytes(t.write[:2], "big")
            for i, b in enumerate(t.write[2:]):
                self.regs[self.pointer + i] = b
            if len(t.write) > 2:
                self.pointer += len(t.write) - 2
        out = bytes(self.regs.get(self.pointer + i, 0) for i in range(t.read_len))
        self.pointer += t.read_len
        return True, out

    def csi_format(self, f):
        print(f"CSI host: {'capturing' if f['capturing'] else 'stopped'}, {f['lanes']} lane(s)", flush=True)
        if f["capturing"] and not self.capturing:
            self.next_at = self.bus.now + self.period
        self.capturing, self.session = f["capturing"], f["session"]
        self.hold = True

    def tick(self, now):
        if self.hold:                                # frames follow the next heartbeat
            self.hold = False
            return
        while self.capturing and self.next_at <= now + LEAD * self.period:
            for msg in wire.cam_frame(self.frames[self.sent % len(self.frames)], due_ns=self.next_at,
                                      session=self.session):
                self.bus.send(msg)
            self.sent += 1
            self.next_at += self.period

    def closed(self):
        print(f"sent {self.sent} frames", flush=True)
