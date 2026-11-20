# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import struct
import sys

from .. import wire
from ..device import Device
from ..picture import Picture

BYTES = {1: 2, 2: 3}     # DpiPixelFormat: RGB565_LE, BGR888


def rgb_row(row, bpp):
    """A row of the frame buffer as red, green, blue: RGB565 little-endian
    at 2 bytes a pixel, blue, green, red at 3."""
    if bpp == 3:
        rgb = bytearray(row)
        rgb[0::3], rgb[2::3] = row[2::3], row[0::3]
        return rgb
    out = bytearray()
    for (px,) in struct.iter_unpack("<H", row):
        r, g, b = px >> 11, (px >> 5) & 0x3F, px & 0x1F
        out += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return out


class DsiView(Picture, Device):
    """A MIPI-DSI panel: what ESP32-P4's DSI host shows, drawn into a PNG.

    Registers as the panel on a virtual channel of the DSI link. The
    emulator sends the commands the firmware gives the panel, the video
    stream's format, and the rows of each frame that changed; the PNG is
    rewritten when rows arrived, at most every --every seconds, and once
    more when the emulator leaves:

        esp-emu --chip esp32p4 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5595 &
        python3 tools/cosim-tcp.py 5595 dsi-view --out dsi.png

    The picture is the frame buffer as the firmware wrote it: a panel's own
    state (sleep, display off, brightness) is not applied; --commands
    prints the packets instead.
    """

    name = "dsi-view"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--vc", type=int, default=0, help="the panel's virtual channel (default 0)")
        Picture.picture_arguments(ap, "dsi.png")
        ap.add_argument("--commands", action="store_true", help="print each packet sent to the panel")

    def __init__(self, args):
        super().__init__(args)
        self.width = self.height = self.bpp = 0
        self.fb = bytearray()
        self.frame_no = 0

    def claims(self):
        return [wire.dsi_claim(self.args.vc)]

    def dsi(self, ev):
        if ev[0] == "command":
            if self.args.commands:
                print(f"dt=0x{ev[1]:02X} {ev[2].hex(' ')}", flush=True)
        elif ev[0] == "format":
            _, streaming, w, h, fmt = ev
            print(f"stream {'on' if streaming else 'off'}: {w}x{h}", flush=True)
            if fmt not in BYTES:
                sys.exit(f"pixel format {fmt} is not one this device draws")
            if (w, h, BYTES[fmt]) != (self.width, self.height, self.bpp):
                self.width, self.height, self.bpp = w, h, BYTES[fmt]
                self.fb = bytearray(w * h * self.bpp)
        elif ev[0] == "rows" and self.width:
            _, self.frame_no, y, _rows, pixels = ev
            at = y * self.width * self.bpp
            if at + len(pixels) > len(self.fb):
                sys.exit(f"rows {y}.. run past the {self.width}x{self.height} frame")
            self.fb[at:at + len(pixels)] = pixels
            self.dirty = True

    def render(self):
        if not self.fb:
            return None
        stride = self.width * self.bpp
        rows = (rgb_row(self.fb[y * stride:(y + 1) * stride], self.bpp) for y in range(self.height))
        return self.width, self.height, rows

    def caption(self):
        return f"{self.width}x{self.height}, frame {self.frame_no}"
