# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from .. import wire
from ..device import Device


class SpiMaster(Device):
    """An SPI master with the chip as its slave: the sender of IDF's spi_slave example.

    Claims the chip select the firmware's spi_slave driver listens on and
    watches the handshake pad. Each time the firmware raises it (a
    transaction is loaded), it clocks one transfer and prints what the
    slave sent back, as examples/peripherals/spi_slave/sender does:

        esp-emu --chip esp32c6 --firmware receiver.bin --cosim-tcp 127.0.0.1:5590 &
        python3 tools/cosim-tcp.py 5590 spi-master --cs 14 --handshake 2

    With --handshake -1 it does not wait for a pad: it clocks one transfer
    every --every-ms of emulated time.
    """

    name = "spi-master"
    unfinished = "the emulator left before every transfer was answered"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--cs", type=int, default=14, help="the chip-select GPIO (default %(default)s)")
        ap.add_argument("--handshake", type=int, default=2,
                        help="the GPIO the slave raises when ready, -1 for none (default %(default)s)")
        ap.add_argument("--len", type=int, default=128, help="bytes a transfer (default %(default)s)")
        ap.add_argument("--hz", type=int, default=5_000_000, help="the clock, in Hz (default %(default)s)")
        ap.add_argument("--count", type=int, default=5, help="transfers before leaving, 0 for no end (default %(default)s)")
        ap.add_argument("--every-ms", type=float, default=100, help="the period without a handshake pad")

    def claims(self):
        claims = [wire.spi_master_claim(self.args.cs)]
        if self.args.handshake >= 0:
            claims.append(wire.gpio_claim(0, 1 << self.args.handshake))
        return claims

    def attached(self, ack):
        self.sent = self.answered = 0
        self.last = b""
        self.next_at = None
        # The firmware may have raised the pad before this attached.
        self.ready = self.args.handshake >= 0 and bool(wire.pin_levels(ack) >> self.args.handshake & 1)

    def start(self, now):
        if self.args.handshake < 0:
            self.next_at = now
        elif self.ready:
            self.clock()

    def tick(self, now):
        if self.next_at is not None and now >= self.next_at and self.sent == self.answered:
            # Periodic without a handshake pad; with one, a single retry of
            # a transfer that found no slave while the pad is still high.
            self.next_at = now + int(self.args.every_ms * 1e6) if self.args.handshake < 0 else None
            if self.args.handshake < 0 or self.ready:
                self.clock()

    def gpio(self, pin, level):
        if pin != self.args.handshake:
            return
        self.ready = level
        if level and self.sent == self.answered and self.bus.started:
            self.clock()

    def clock(self):
        if self.args.count and self.sent >= self.args.count:
            return
        text = f'Sender, transmission no. {self.sent:04d}. Last time, I received: "{self.last.decode(errors="replace")}"'
        tx = text.encode()[:self.args.len - 1].ljust(self.args.len, b"\0")
        self.sent += 1
        self.bus.spi_transfer(tx, self.args.hz, ident=self.sent)

    def spi_result(self, ident, selected, rx, end_ns):
        if not selected:
            # The firmware has no slave on the pad yet: not one of the count.
            print(f"no slave on GPIO {self.args.cs} yet", flush=True)
            self.sent -= 1
            # The pad will not rise again for this transaction: try again.
            self.next_at = self.bus.now + int(self.args.every_ms * 1e6)
            return
        self.answered += 1
        self.last = rx.split(b"\0")[0][:40]
        print(f"transfer {ident}: {len(rx)} bytes, chip select rose at {end_ns / 1e6:.3f} ms: "
              f"{self.last.decode(errors='replace')!r}...", flush=True)
        if self.args.count and self.answered >= self.args.count:
            self.finish()
