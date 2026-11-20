# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import time

from .. import wire
from ..device import Device


class Clock(Device):
    """A clock holder: the chip runs only as fast as this lets it.

    Claims the chip's clock and grants it emulated time one quantum at a
    time, each when the chip has used up the one before. With --rate the
    grants are paced against the host's clock, so the chip runs at that
    fraction of real time (1 is real time, 0.5 half speed); two emulators
    each with one of these at the same rate advance together. Without it the
    chip runs as fast as the host allows, a quantum at a time.

        esp-emu --chip esp32c3 --firmware app.bin --cosim-tcp 127.0.0.1:5590 &
        python3 tools/cosim-tcp.py 5590 clock --rate 1 --quantum-ms 1

    --for-ms stops the pacing after that much emulated time has been
    granted: the chip is then let run on, and the device has finished.

    A paced clock sleeps between grants, and a session has one receive loop:
    run it on a connection of its own, not joined to other devices with '+'.
    """

    name = "clock"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--quantum-ms", type=float, default=1.0, help="emulated time a grant (default %(default)s)")
        ap.add_argument("--rate", type=float, default=0.0,
                        help="emulated seconds per host second, 0 for as fast as the host allows (default %(default)s)")
        ap.add_argument("--for-ms", type=float, default=0.0, help="stop after granting this much, 0 for no end")

    def claims(self):
        return [wire.time_claim()]

    def attached(self, ack):
        self.quantum_ns = int(self.args.quantum_ms * 1e6)
        if self.quantum_ns < 1:
            self.fail("--quantum-ms must be at least a nanosecond")
        self.granted_ns = 0
        self.began = None           # host time of the first grant
        self.first_at = None        # the chip's time at its first stop

    def time_waiting(self, at):
        if self.finished:
            return
        if self.first_at is None:
            self.first_at, self.began = at, time.monotonic()
        if self.args.for_ms and self.granted_ns >= self.args.for_ms * 1e6:
            took = time.monotonic() - self.began
            print(f"granted {self.granted_ns / 1e6:.3f} ms; the chip ran {(at - self.first_at) / 1e6:.3f} ms "
                  f"in {took:.3f} s of host time", flush=True)
            # The claim lasts as long as the connection: grant what the chip
            # will not use up, so a session that goes on does not hold it.
            self.bus.grant(1 << 62)
            self.finish()
            return
        if self.args.rate > 0:
            # The grant that takes the chip to granted_ns + quantum is due
            # when the host has seen granted_ns / rate.
            due = self.began + self.granted_ns / 1e9 / self.args.rate
            delay = due - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        self.granted_ns += self.quantum_ns
        self.bus.grant(self.quantum_ns)
