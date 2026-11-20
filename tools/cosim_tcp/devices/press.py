# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from .. import wire
from ..device import Device


class Press(Device):
    """A push button: presses a GPIO low for a while, once.

    Registers as the model driving the pad, waits for a heartbeat, and
    presses it for --ms of emulated time:

        esp-emu --chip esp32c3 --firmware app.bin --cosim-tcp 127.0.0.1:5590 &
        python3 tools/cosim-tcp.py 5590 press --pin 9
    """

    name = "press"
    unfinished = "the emulator left before its first heartbeat"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--pin", type=int, default=9, help="the button's GPIO (default %(default)s)")
        ap.add_argument("--ms", type=float, default=300, help="how long it is held (default %(default)s)")

    def claims(self):
        return [wire.gpio_claim(1 << self.args.pin)]

    def start(self, now):
        self.release = now + int(self.args.ms * 1e6)
        self.unfinished = "the emulator left before the release landed"
        self.bus.drive(self.args.pin, False, now)
        self.bus.drive(self.args.pin, True, self.release)
        print(f"pressed GPIO {self.args.pin} at {now / 1e6:.1f} ms for {self.args.ms:.0f} ms", flush=True)

    def tick(self, now):
        # Leaving lets go of the pad, so stay until the release has landed.
        if now > self.release:
            self.finish()
