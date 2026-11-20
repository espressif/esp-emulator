# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from .. import wire
from ..device import Device


class Pca9548a:
    """The control byte: a set bit selects that channel."""

    def __init__(self):
        self.ctrl = 0

    def xfer(self, w, n):
        if w:
            self.ctrl = w[0]
        return True, bytes([self.ctrl]) * n

    @property
    def channel(self):
        return self.ctrl.bit_length() - 1 if self.ctrl else None


class Pcf8575:
    """A 16-bit quasi-bidirectional latch: two bytes written set it, two
    read return it (an input pulled low would clear its bit)."""

    def __init__(self):
        self.latch = 0xFFFF

    def xfer(self, w, n):
        if len(w) >= 2:
            self.latch = w[0] | (w[1] << 8)
        out = bytes([self.latch & 0xFF, self.latch >> 8])
        return True, (out * (n // 2 + 1))[:n]


class Vl53l0x:
    """A register file with the few registers a driver's init polls, and a
    distance at RESULT_RANGE_STATUS + 10 (0x1E, 0x1F) that moves. The
    register pointer outlives a transaction, as on the part: a write of the
    index then a bare read, two transactions with a STOP between, reads
    from that index."""

    def __init__(self, mm):
        self.mm = mm
        self.reg = {0xC0: 0xEE, 0xC1: 0xAA, 0xC2: 0x10,   # model, revision IDs
                    0x91: 0x3C,                           # stop variable
                    0x89: 0x01, 0x92: 0x2A}               # VHV config, SPAD count
        self.cursor = 0
        self.reads = 0

    def xfer(self, w, n):
        if w:
            for i, b in enumerate(w[1:]):
                self.reg[(w[0] + i) & 0xFF] = b
            self.cursor = (w[0] + len(w) - 1) & 0xFF
            if n == 0:
                return True, b""
        out = bytearray()
        for _ in range(n):
            a = self.cursor
            self.cursor = (self.cursor + 1) & 0xFF
            if a == 0x1E:
                out.append(self.mm >> 8)
            elif a == 0x1F:
                out.append(self.mm & 0xFF)
                self.reads += 1
                self.mm = 200 + (self.mm + 7) % 1800
            elif a == 0x83:                               # polled until non-zero
                out.append(0x01)
            elif a == 0x13:                               # interrupt status: data ready
                out.append(0x07)
            else:
                out.append(self.reg.get(a, 0x00))
        return True, bytes(out)


class I2cBoard(Device):
    """An I2C board: a mux, an expander and eight ranging sensors.

    A PCA9548A mux at 0x70, a PCF8575 expander at 0x20 holding the
    sensors' XSHUT lines, and a VL53L0X ranging sensor at 0x29 behind each of the
    mux's eight channels. One device is the whole board: it claims the three
    addresses and routes 0x29 to the sensor on the channel the mux selects, so
    eight devices share one address the way they do on the wire.

        python3 tools/cosim-tcp.py 5593 i2c-board --scl 7 --sda 6 &
        esp-emu --chip esp32c3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5593

    Start the device first: the emulator does not wait for a consumer, and a
    firmware that scans the bus at boot would find nothing. The pads are the
    ones the firmware gives its bus (XIAO ESP32C3: SDA 6, SCL 7).

    A sensor whose XSHUT bit is low in the expander's latch is in reset and
    NACKs, as the real part does. --absent N leaves channel N's sensor off the
    board, so its init fails while the other seven range. The VL53L0X here is
    the register map a driver's init and continuous ranging touch (the Pololu
    and Adafruit libraries): the ID registers, the status bits the driver polls,
    and a distance that moves between reads.
    """

    name = "i2c-board"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--scl", type=int, default=7, help="the bus's SCL pad")
        ap.add_argument("--sda", type=int, default=6, help="the bus's SDA pad")
        ap.add_argument("--absent", type=int, metavar="CH", help="no sensor on this mux channel")

    def __init__(self, args):
        super().__init__(args)
        self.mux, self.pcf = Pca9548a(), Pcf8575()
        self.tof = [Vl53l0x(300 + 100 * i) for i in range(8)]
        self.count = 0

    def claims(self):
        return [wire.i2c_claim(a, self.args.scl, self.args.sda) for a in (0x70, 0x20, 0x29)]

    def attached(self, ack):
        print("board: attached", flush=True)

    def i2c(self, t):
        self.count += 1
        if t.addr == 0x70:
            return self.mux.xfer(t.write, t.read_len)
        if t.addr == 0x20:
            return self.pcf.xfer(t.write, t.read_len)
        ch = self.mux.channel
        in_reset = ch is not None and not (self.pcf.latch >> ch) & 1
        if ch is None or in_reset or ch == self.args.absent:
            return False, b""
        return self.tof[ch].xfer(t.write, t.read_len)

    def closed(self):
        print(f"board: {self.count} transactions; ranging reads per channel: {[x.reads for x in self.tof]}", flush=True)
