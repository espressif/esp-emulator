# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""A display's frame buffer as a PNG file, rewritten while it changes."""
import struct
import time
import zlib


def png(width, height, rows):
    """A PNG of `height` rows, each `width` pixels of red, green, blue."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        raw += row

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


class Picture:
    """What a display device shares: --out and --every, and a file rewritten
    when the picture is `dirty`, at most every --every seconds and once more
    at the end. The device has `render()`, returning (width, height, rows)
    or None while there is nothing to show, and `caption()`."""

    dirty = False
    written = 0
    last_write = None

    @staticmethod
    def picture_arguments(ap, default):
        ap.add_argument("--out", default=default, help="where the picture goes (default %(default)s)")
        ap.add_argument("--every", type=float, default=0.5, help="seconds between rewrites (default %(default)s)")

    def write_picture(self):
        shown = self.render()
        if shown is None:
            print("no frame was shown", flush=True)
            return
        with open(self.args.out, "wb") as f:
            f.write(png(*shown))
        self.written += 1
        self.dirty = False
        self.last_write = time.monotonic()
        print(f"{self.args.out}: {self.caption()}", flush=True)

    def idle(self):
        if self.last_write is None:
            self.last_write = time.monotonic()
        if self.dirty and time.monotonic() - self.last_write >= self.args.every:
            self.write_picture()

    def closed(self):
        if self.dirty or not self.written:
            self.write_picture()
