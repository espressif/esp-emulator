# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import math

from .. import wire
from ..device import Device


class Adc(Device):
    """A voltage source on an analog pad: a sine, a square or a constant.

    Sends the signal ahead of the chip as AnalogWave pieces, sampled --rate
    times a second, --piece-ms a message, keeping --ahead-ms sent ahead of
    the chip's clock, so each of the firmware's conversions reads the signal
    at its own instant:

        esp-emu --chip esp32c6 --firmware app.bin --cosim-tcp 127.0.0.1:5590 &
        python3 tools/cosim-tcp.py 5590 adc --pin 3 --sine 1650:1000:50

    --sine and --square take OFFSET:AMPLITUDE:HZ, in millivolts and hertz;
    --mv is a constant. A chip reset starts the signal again on the new
    timeline.
    """

    name = "adc"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--pin", type=int, required=True, help="the analog pad's GPIO")
        shape = ap.add_mutually_exclusive_group()
        shape.add_argument("--mv", type=float, help="a constant voltage")
        shape.add_argument("--sine", metavar="OFFSET:AMPLITUDE:HZ", help="a sine")
        shape.add_argument("--square", metavar="OFFSET:AMPLITUDE:HZ", help="a square wave")
        ap.add_argument("--rate", type=float, default=10000, help="samples a second (default %(default)s)")
        ap.add_argument("--piece-ms", type=float, default=5, help="emulated time a message (default %(default)s)")
        ap.add_argument("--ahead-ms", type=float, default=20,
                        help="how far ahead of the chip's clock to send (default %(default)s)")

    def claims(self):
        return [wire.analog_claim(self.args.pin)]

    def attached(self, ack):
        a = self.args
        if a.rate <= 0:
            self.fail("--rate must be above 0")
            return
        self.period_ns = max(1, round(1e9 / a.rate))
        self.per_piece = max(1, min(4096, round(a.piece_ms * 1e6 / self.period_ns)))
        self.ahead_ns = int(a.ahead_ms * 1e6)
        spec = a.sine or a.square
        if spec:
            try:
                self.offset, self.amplitude, self.hz = (float(v) for v in spec.split(":"))
            except ValueError:
                self.fail(f"{spec!r} is not OFFSET:AMPLITUDE:HZ")
                return
        else:
            self.offset, self.amplitude, self.hz = (1650 if a.mv is None else a.mv), 0.0, 0.0
        self.square = a.square is not None
        self.origin = None      # emulated time the signal starts at
        self.sent_to = None     # emulated time the pieces sent so far reach
        self.waves = 0
        self.sync = {}          # the last heartbeat's counters

    def level(self, t_ns):
        phase = math.sin(2 * math.pi * self.hz * (t_ns - self.origin) / 1e9)
        if self.square:
            phase = 1.0 if phase >= 0 else -1.0
        return round(min(3300, max(0, self.offset + self.amplitude * phase)))

    def start(self, now):
        self.tick(now)

    def tick(self, now):
        if self.finished:
            return
        if self.origin is None:
            self.origin = self.sent_to = now
        # Behind the chip (a slow host): what has passed is not worth sending.
        self.sent_to = max(self.sent_to, now)
        while self.sent_to < now + self.ahead_ns:
            at = self.sent_to
            mvs = [self.level(at + k * self.period_ns) for k in range(self.per_piece)]
            self.bus.send(wire.analog_wave(at, self.args.pin, self.period_ns, mvs))
            self.sent_to = at + self.per_piece * self.period_ns
            self.waves += 1

    def reset(self):
        self.origin = self.sent_to = None

    def clock_sync(self, sync):
        self.sync = sync

    def closed(self):
        if self.failure is None and self.sent_to is not None:
            print(f"GPIO {self.args.pin}: {self.waves} AnalogWave messages, {self.waves * self.per_piece} voltages, "
                  f"up to {self.sent_to / 1e6:.1f} ms; at the last heartbeat the emulator counted "
                  f"{self.sync.get('events_fired_late', 0)} late and {self.sync.get('scheduled_dropped', 0)} refused",
                  flush=True)
