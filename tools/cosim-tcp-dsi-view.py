#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""A MIPI-DSI panel on the co-simulation bus over --cosim-tcp, with no
protobuf library: what the ESP32-P4's DSI host shows, drawn into a PNG.

Registers as the panel on a virtual channel of the DSI link. The emulator
sends the commands the firmware gives the panel, the video stream's format,
and the rows of each frame that changed; the PNG is rewritten when rows
arrived, at most every --every seconds, and once more when the emulator
leaves:

    esp-emu --chip esp32p4 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5595 &
    python3 tools/cosim-tcp-dsi-view.py 5595 --out dsi.png

The picture is the frame buffer as the firmware wrote it: a panel's own
state (sleep, display off, brightness) is not applied; --commands prints the
packets instead. The framing and the messages are in cosim_tcp.py beside
this script.
"""
import argparse
import struct
import sys
import time
import zlib

from cosim_tcp import accepted, connect, dsi_claim, dsi_event, recv, register, send

BYTES = {1: 2, 2: 3}     # DpiPixelFormat: RGB565_LE, BGR888


def png(width, height, bpp, fb):
    """A frame as a PNG: RGB565 little-endian at 2 bytes a pixel, blue,
    green, red at 3."""
    raw = bytearray()
    for y in range(height):
        row = fb[y * width * bpp:(y + 1) * width * bpp]
        raw.append(0)
        if bpp == 3:
            rgb = bytearray(row)
            rgb[0::3], rgb[2::3] = row[2::3], row[0::3]
            raw += rgb
            continue
        for (px,) in struct.iter_unpack("<H", row):
            r, g, b = px >> 11, (px >> 5) & 0x3F, px & 0x1F
            raw += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port", type=int, help="the --cosim-tcp port")
    ap.add_argument("--vc", type=int, default=0, help="the panel's virtual channel (default 0)")
    ap.add_argument("--out", default="dsi.png", help="where the picture goes (default dsi.png)")
    ap.add_argument("--every", type=float, default=0.5, help="seconds between rewrites (default 0.5)")
    ap.add_argument("--commands", action="store_true", help="print each packet sent to the panel")
    ap.add_argument("--name", default="dsi-view.py")
    a = ap.parse_args()

    s = connect(a.port)
    send(s, register(a.name, dsi_claim(a.vc)))
    topic, ack = recv(s)
    ok, reason = accepted(ack)
    if not ok:
        sys.exit(f"refused on {topic}: {reason or ack!r}")

    width = height = bpp = 0
    fb = bytearray()
    frame, dirty, written = 0, False, 0
    last = time.monotonic()

    def write():
        nonlocal dirty, written, last
        if not fb:
            print("no frame was shown", flush=True)
            return
        with open(a.out, "wb") as f:
            f.write(png(width, height, bpp, fb))
        written += 1
        dirty = False
        last = time.monotonic()
        print(f"{a.out}: {width}x{height}, frame {frame}", flush=True)

    try:
        while True:
            topic, payload = recv(s)
            if topic is None:
                break
            ev = dsi_event(payload)
            if ev is None:
                pass
            elif ev[0] == "command":
                if a.commands:
                    print(f"dt=0x{ev[1]:02X} {ev[2].hex(' ')}", flush=True)
            elif ev[0] == "format":
                _, streaming, w, h, fmt = ev
                print(f"stream {'on' if streaming else 'off'}: {w}x{h}", flush=True)
                if fmt not in BYTES:
                    sys.exit(f"pixel format {fmt} is not one this script draws")
                if (w, h, BYTES[fmt]) != (width, height, bpp):
                    width, height, bpp = w, h, BYTES[fmt]
                    fb = bytearray(w * h * bpp)
            elif ev[0] == "rows" and width:
                _, frame, y, _rows, pixels = ev
                at = y * width * bpp
                if at + len(pixels) > len(fb):
                    sys.exit(f"rows {y}.. run past the {width}x{height} frame")
                fb[at:at + len(pixels)] = pixels
                dirty = True
            if dirty and time.monotonic() - last >= a.every:
                write()
    except KeyboardInterrupt:
        pass
    if dirty or not written:
        write()


if __name__ == "__main__":
    main()
