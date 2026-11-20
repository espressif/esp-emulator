# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from .. import wire
from ..device import Device
from ..picture import Picture

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
        self.head = b""                     # QSPI: the frame's opcode word so far
        self.xs, self.xe, self.ys, self.ye = 0, MAX_SIDE - 1, 0, MAX_SIDE - 1
        self.x, self.y = 0, 0
        self.width, self.height = 0, 0      # the extent pixels have reached
        self.inverted = False
        self.odd = b""                       # half a pixel left from a chunk
        self.windows = 0                     # RAMWR windows filled so far
        for i in range(0, len(self.fb)):
            self.fb[i] = 0

    def transfer(self, t):
        """One SpiTransfer: its command phase, then its data bytes with
        D/C as it was clocked."""
        if 6 in t:                           # the command phase
            phase = wire.decode(t[6])
            data = phase.get(3, b"")
            if data:
                self.command(data[-1])
        tx = t.get(3, b"")
        if self.dc_pin is None:              # QSPI: the opcode word opens a frame
            if len(self.head) < 4:
                take = 4 - len(self.head)
                self.head, tx = self.head + tx[:take], tx[take:]
                if len(self.head) == 4:
                    self.command(self.head[2])
            if tx and len(self.head) == 4:
                self.data(tx)
            if not t.get(12, 0):             # chip select released: the frame ends
                self.head = b""
            return
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


class LcdView(Picture, Device):
    """An SPI display: the ST7789 / ILI9341 command set, drawn into a PNG.

    Registers as the device on the panel's chip select and follows every
    transfer the firmware clocks at it. A command arrives in the
    controller's command phase (esp_lcd) or as a data byte with D/C low (a
    driver that bit-bangs D/C); pixels arrive with D/C high after RAMWR,
    into the window CASET and RASET set. A QSPI panel (CO5300, SH8601,
    ST77916) has no D/C line: leave --dc out, and each chip-select frame
    opens with the opcode word (0x02 or 0x32, 0, command, 0). The PNG is
    rewritten whenever a window has been filled, at most every --every
    seconds, and once more when the emulator leaves:

        esp-emu --chip esp32s3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5591 &
        python3 tools/cosim-tcp.py 5591 lcd-view --cs 22 --dc 21 --out lcd.png

    A read the firmware makes of the panel (RDDID, RDDST) is answered with
    zeros.
    """

    name = "lcd-view"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--cs", type=int, required=True, help="the panel's chip-select GPIO")
        ap.add_argument("--dc", type=int, help="the panel's D/C GPIO; none for a QSPI panel")
        Picture.picture_arguments(ap, "lcd.png")

    def __init__(self, args):
        super().__init__(args)
        self.panel = Panel(args.dc)
        self.transfers = 0

    def claims(self):
        return [wire.spi_claim(self.args.cs)]

    def reset(self):
        self.panel.reset()
        self.dirty = False                   # the picture went with the reset

    def spi(self, t):
        self.transfers += 1
        before = self.panel.windows
        self.panel.transfer(t)
        if self.panel.windows > before:
            self.dirty = True
        elif self.panel.windows < before:    # SWRESET: nothing left to write
            self.dirty = False

    def render(self):
        p = self.panel
        w, h = max(p.width, 1), max(p.height, 1)
        return w, h, (p.fb[y * MAX_SIDE * 3:(y * MAX_SIDE + w) * 3] for y in range(h))

    def caption(self):
        p = self.panel
        return f"{p.width}x{p.height}, {p.windows} windows, {self.transfers} transfers"
