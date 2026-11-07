#!/usr/bin/env python3
"""An SPI display on the co-simulation bus over --cosim-tcp, with no protobuf
library: the ST7789 / ILI9341 command set, drawn into a PNG.

Registers as the device on the panel's chip select and follows every transfer
the firmware clocks at it. A command arrives in the controller's command phase
(esp_lcd) or as a data byte with D/C low (a driver that bit-bangs D/C); pixels
arrive with D/C high after RAMWR, into the window CASET and RASET set. The
PNG is rewritten whenever a window has been filled, at most every --every
seconds, and once more when the emulator leaves:

    esp-emu --chip esp32s3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5591 &
    python3 tools/cosim-lcd-view.py 5591 --cs 22 --dc 21 --out lcd.png

A read the firmware makes of the panel (RDDID, RDDST) is answered with zeros.
The framing and the messages are in cosim_tcp.py beside this script.
"""
import argparse
import struct
import time
import sys
import zlib

from cosim_tcp import accepted, bus_reply, connect, decode, recv, register, send, spi_claim

MAX_SIDE = 512          # the largest panel drawn, in either direction


# ---------------------------------------------------------------- the panel

class Panel:
    """The command set a ST7789 or ILI9341 driver uses: the window, the
    write, reset, inversion, and the rest acknowledged and ignored."""

    def __init__(self, dc_pin):
        self.dc_pin = dc_pin
        self.fb = bytearray(MAX_SIDE * MAX_SIDE * 3)
        self.reset()

    def reset(self):
        self.cmd = None
        self.args = bytearray()
        self.xs, self.xe, self.ys, self.ye = 0, MAX_SIDE - 1, 0, MAX_SIDE - 1
        self.x, self.y = 0, 0
        self.width, self.height = 0, 0      # the extent pixels have reached
        self.inverted = False
        self.odd = b""                       # half a pixel left from a chunk
        self.windows = 0                     # RAMWR windows filled so far
        self.dirty = False
        for i in range(0, len(self.fb)):
            self.fb[i] = 0

    def transfer(self, t):
        """One SpiTransfer: its command phase, then its data bytes with
        D/C as it was clocked."""
        if 6 in t:                           # the command phase
            phase = decode(t[6])
            data = phase.get(3, b"")
            if data:
                self.command(data[-1])
        tx = t.get(3, b"")
        dc_high = (t.get(5, 0) >> self.dc_pin) & 1
        if not tx:
            return
        if dc_high:
            self.data(tx)
        else:
            for b in tx:
                self.command(b)

    def command(self, c):
        self.flush_args()
        self.cmd = c
        self.args = bytearray()
        if c == 0x01:                        # SWRESET
            self.reset()
        elif c == 0x2C:                      # RAMWR: back to the window's corner
            self.x, self.y = self.xs, self.ys
            self.odd = b""
        elif c == 0x3C:                      # RAMWRC: carry on
            pass
        elif c == 0x21:
            self.inverted = True
        elif c == 0x20:
            self.inverted = False

    def data(self, tx):
        if self.cmd in (0x2C, 0x3C):
            self.pixels(tx)
        else:
            self.args += tx

    def flush_args(self):
        """CASET / RASET take their four bytes once the next command comes,
        so a driver splitting the parameters over transfers still lands."""
        a = self.args
        if self.cmd == 0x2A and len(a) >= 4:
            self.xs, self.xe = (a[0] << 8 | a[1]), (a[2] << 8 | a[3])
        elif self.cmd == 0x2B and len(a) >= 4:
            self.ys, self.ye = (a[0] << 8 | a[1]), (a[2] << 8 | a[3])

    def pixels(self, tx):
        buf = self.odd + tx
        n = len(buf) & ~1
        self.odd = buf[n:]
        fb, inv = self.fb, self.inverted
        for i in range(0, n, 2):
            p = buf[i] << 8 | buf[i + 1]
            if inv:
                p ^= 0xFFFF
            if self.x < MAX_SIDE and self.y < MAX_SIDE:
                o = (self.y * MAX_SIDE + self.x) * 3
                fb[o] = (p >> 8) & 0xF8 | (p >> 13)
                fb[o + 1] = (p >> 3) & 0xFC | ((p >> 9) & 0x3)
                fb[o + 2] = (p << 3) & 0xF8 | ((p >> 2) & 0x7)
                # The picture is as large as the pixels written, not the
                # window an init table set: an ILI9341's clears all 320 rows.
                if self.x >= self.width:
                    self.width = self.x + 1
                if self.y >= self.height:
                    self.height = self.y + 1
            self.x += 1
            if self.x > self.xe:
                self.x = self.xs
                self.y += 1
                if self.y > self.ye:
                    self.y = self.ys
                    self.windows += 1
                    self.dirty = True

    def png(self):
        w, h = max(self.width, 1), max(self.height, 1)
        raw = b"".join(b"\x00" + bytes(self.fb[y * MAX_SIDE * 3:(y * MAX_SIDE + w) * 3]) for y in range(h))

        def chunk(kind, body):
            return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------- the run

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port", type=int, help="the --cosim-tcp port")
    ap.add_argument("--cs", type=int, required=True, help="the panel's chip-select GPIO")
    ap.add_argument("--dc", type=int, required=True, help="the panel's D/C GPIO")
    ap.add_argument("--out", default="lcd.png", help="where the picture goes (default lcd.png)")
    ap.add_argument("--every", type=float, default=0.5, help="seconds between rewrites (default 0.5)")
    ap.add_argument("--name", default="lcd-view.py")
    a = ap.parse_args()

    s = connect(a.port)
    send(s, register(a.name, spi_claim(a.cs)))
    topic, ack = recv(s)
    ok, reason = accepted(ack)
    if not ok:
        sys.exit(f"refused on {topic}: {reason or ack!r}")

    panel = Panel(a.dc)
    transfers, written = 0, 0
    last = time.monotonic()

    def write():
        nonlocal written, last
        with open(a.out, "wb") as f:
            f.write(panel.png())
        written += 1
        last = time.monotonic()
        panel.dirty = False
        print(f"{a.out}: {panel.width}x{panel.height}, {panel.windows} windows, {transfers} transfers", flush=True)

    try:
        while True:
            topic, payload = recv(s)
            if topic is None:
                break
            msg = decode(payload)
            if topic == "SIM/RESET":
                panel.reset()
            elif 7 in msg:                   # a BusRequest for this consumer
                req = decode(msg[7])
                if 3 not in req:
                    continue
                t = decode(req[3])
                transfers += 1
                panel.transfer(t)
                rx_len = t.get(4, 0)
                if rx_len:
                    send(s, bus_reply(req.get(1, 0), bytes(rx_len)))
            if panel.dirty and time.monotonic() - last >= a.every:
                write()
    except KeyboardInterrupt:
        pass
    if panel.dirty or not written:
        write()


if __name__ == "__main__":
    main()
