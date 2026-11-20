# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from .. import wire
from ..device import Device


class Reset(Device):
    """A reset button on EN: holds the chip in reset for a while, once.

    Registers as a device wired to EN (CHIP_PU), waits for a heartbeat,
    pulls EN low and lets it go --ms of emulated time later (longer if the
    emulator, which does not wait for a socket, has run past that by the
    time the release arrives). The chip runs nothing meanwhile and then
    boots as from power-on:

        esp-emu --chip esp32c3 --firmware app.bin --cosim-tcp 127.0.0.1:5590 &
        python3 tools/cosim-tcp.py 5590 reset --ms 500
    """

    name = "reset"
    unfinished = "the emulator left before its first heartbeat"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--ms", type=float, default=200, help="how long EN is low (default %(default)s)")

    def claims(self):
        return [wire.en_claim()]

    def attached(self, ack):
        self.already_low = wire.en_low(ack)
        self.hold_ns = int(self.args.ms * 1e6)
        self.fell = None

    def start(self, now):
        # The fall may land later than `now`: the emulator did not wait for
        # this script. So the release is timed from the fall it reports.
        self.bus.send(wire.en_drive(now, False))
        self.unfinished = "the emulator left while EN was low"
        # Already low, by another holder: nothing falls, so there is no
        # report to time from. This hold then runs from now.
        if self.already_low:
            print("EN is already low", flush=True)
            self.release_after(now)

    def release_after(self, fell):
        self.fell = fell
        self.bus.send(wire.en_drive(fell + self.hold_ns, True))

    def en(self, at, level):
        print(f"EN {'high' if level else 'low'} at {at / 1e6:.1f} ms", flush=True)
        if not level and self.fell is None and self.bus.started:
            self.release_after(at)

    def reset(self):
        # Leaving lets go of EN, so the device stays until here.
        if self.fell is not None:
            print("the chip reset and boots as from power-on", flush=True)
            self.finish()
