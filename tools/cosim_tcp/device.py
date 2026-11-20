# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""A device on the co-simulation socket as a class, and the loop that runs
any number of them on one connection.

A device subclasses `Device`, names its claims and overrides the events it
cares about; `Session` connects, registers them all as one consumer, decodes
each frame once and hands it to the device it is for. `main` is the command
line over every device in `devices/`: a new peripheral is one class there,
not a script with its own socket loop.
"""
import argparse
import sys

from . import wire

DEVICES = {}        # command-line name: Device subclass


class Device:
    """One device of a board. Every event has a default that does nothing,
    so a subclass has only the ones its peripheral uses. A subclass with a
    `name` is on the command line under it."""

    name = None             # the command-line name
    # What a one-shot device says if the emulator leaves before `finish()`;
    # None for a device that runs for as long as the emulator does.
    unfinished = None

    def __init_subclass__(cls, **kw):
        super().__init_subclass__(**kw)
        if cls.__dict__.get("name"):
            DEVICES[cls.name] = cls

    @staticmethod
    def arguments(ap):
        """Add the device's options to its argparse parser."""

    def __init__(self, args):
        self.args = args
        self.bus = None     # the Session, set before `attached`
        self.finished = False
        self.failure = None

    def claims(self):
        """The wire.*_claim() messages for what the device is wired to. An
        I2C claim's address and an SPI claim also route that bus to it."""
        return []

    def finish(self):
        """The device has done its job: the session ends once all have."""
        self.finished = True

    def fail(self, why):
        """The device could not do its job: the tool exits with `why`."""
        self.failure = why
        self.finished = True

    # -- events, in the order a run brings them --------------------------
    def attached(self, ack):
        """Registered: `ack` is the RegisterAck, for wire.en_low and such."""

    def start(self, now):
        """The first heartbeat: `now` is emulated time in ns."""

    def tick(self, now):
        """Every later frame that carries emulated time, heartbeats included."""

    def gpio(self, pin, level):
        """A watched pad changed."""

    def edges(self, pin, first_level, widths_ns, at):
        """A run of edges of a pad claimed in `edge_pins`: `first_level` at
        `at` ns, toggling after each width."""

    def i2c(self, t):
        """A `wire.I2cRequest` for an address it claimed: return (ack, data)."""
        return False, b""

    def spi(self, t):
        """A decoded SpiTransfer on the chip select it claimed: return the
        bytes a read gets (None: zeros)."""

    def spi_result(self, ident, selected, rx, end_ns):
        """What a transfer it clocked as the SPI master read back."""

    def time_waiting(self, at):
        """The chip used up what a clock holder granted and stopped at `at` ns."""

    def i2s_format(self, receive, rate, channels, bits):
        """The transmit or receive direction's sample format."""

    def i2s_out(self, pcm):
        """Samples the chip transmitted."""

    def dvp_format(self, fmt):
        """The camera receiver's state, as `wire.dvp_format` gives it."""

    def csi_format(self, fmt):
        """The CSI host's state, as `wire.csi_format` gives it."""

    def dsi(self, event):
        """A DSI command, format or rows tuple, as `wire.dsi_event` gives."""

    def en(self, at, level):
        """The chip's EN pin changed at `at` ns."""

    def reset(self):
        """The chip reset."""

    def idle(self):
        """After every frame, for work paced by the host's clock."""

    def closed(self):
        """The emulator left, or the session ended."""


class Session:
    """One connection: the devices are one consumer with all their claims."""

    def __init__(self, port, devices, host="127.0.0.1", name="cosim-tcp.py"):
        self.devices = devices
        self.now = 0                # emulated time of the last frame that had one
        self.started = False
        self.i2c = {}               # 7-bit address: the device that claimed it
        self.spi = None             # the device that claimed a chip select
        claims = []
        for d in devices:
            for claim in d.claims():
                self.route(d, wire.decode(claim))
                claims.append(claim)
        self.sock = wire.connect(port, host)
        self.send(wire.register(name, *claims))
        topic, ack = wire.recv(self.sock)
        ok, reason = wire.accepted(ack)
        if not ok:
            sys.exit(f"refused on {topic}: {reason or ack!r}")
        for d in devices:
            d.bus = self
            d.attached(ack)

    def route(self, dev, claim):
        """Note which bus requests a device's claim brings it. A request
        names no device of a consumer, so what cannot be told apart is
        refused here: run those as two connections."""
        if 2 in claim:                               # Claim.i2c
            addr = wire.decode(claim[2]).get(1, 0)
            if self.i2c.setdefault(addr, dev) is not dev:
                sys.exit(f"{dev.name} and {self.i2c[addr].name} both claim I2C address 0x{addr:02X}")
        elif 3 in claim:                             # Claim.spi
            if self.spi not in (None, dev):
                sys.exit(f"{dev.name} and {self.spi.name} both claim an SPI chip select")
            self.spi = dev

    def send(self, msg):
        wire.send(self.sock, msg)

    def drive(self, pin, level, at=None):
        """Drive a claimed pad at `at` ns of emulated time (default: now)."""
        self.send(wire.gpio_drive(self.now if at is None else at, pin, level))

    def spi_transfer(self, tx, sck_hz=0, at=None, ident=0, hold_cs=False):
        """Clock `tx` into the chip as its SPI master at `at` ns (default:
        now); the answer comes to `spi_result`, quoting `ident`. With
        `hold_cs` chip select stays low and the next call goes on with the
        same transfer."""
        self.send(wire.spi_master_transfer(self.now if at is None else at, ident, tx, sck_hz, hold_cs))

    def grant(self, advance_ns):
        """As a clock holder, let the chip run `advance_ns` more."""
        self.send(wire.time_grant(advance_ns))

    def run(self):
        """Hand frames to the devices until the emulator leaves or every
        device has finished. Returns what the devices that failed said."""
        try:
            while not all(d.finished for d in self.devices):
                try:
                    topic, payload = wire.recv(self.sock)
                except ConnectionError:              # the emulator exited
                    break
                if topic is None:
                    break
                self.frame(topic, payload)
                self.each("idle")
        except KeyboardInterrupt:
            pass
        finally:
            # Each device's last word, whatever stopped another one.
            for d in self.devices:
                try:
                    d.closed()
                except Exception as e:               # noqa: BLE001
                    d.failure = d.failure or f"{type(e).__name__}: {e}"
                if not d.finished and d.unfinished:
                    d.failure = d.failure or d.unfinished
        return [f"{d.name}: {d.failure}" for d in self.devices if d.failure]

    def frame(self, topic, payload):
        msg = wire.decode(payload)
        timed = wire.SIM_TIME in msg
        if timed:
            self.now = msg[wire.SIM_TIME]
        starting = topic == "SIM/CLOCK" and not self.started
        if starting:
            self.started = True
            self.each("start", self.now)
        if topic == "SIM/RESET":
            self.each("reset")
        for event, decoder, spread in wire.EVENTS:
            got = decoder(msg)
            if got is not None:
                self.each(event, *(got if spread else (got,)))
        self.bus_request(msg)
        if timed and self.started and not starting:
            self.each("tick", self.now)

    def each(self, event, *args):
        for d in self.devices:
            getattr(d, event)(*args)

    def bus_request(self, msg):
        """Hand a BusRequest to the device that claimed its bus and send
        the answer."""
        req = wire.i2c_request(msg)
        if req is not None:
            cid, t = req
            dev = self.i2c.get(t.addr)
            ack, data = dev.i2c(t) if dev else (False, b"")
            self.send(wire.i2c_reply(cid, data, ack))
            return
        req = wire.spi_request(msg)
        if req is not None:
            cid, t = req
            data = self.spi.spi(t) if self.spi else None
            rx_len = t.get(4, 0)                     # SpiTransfer.rx_len
            if rx_len:
                self.send(wire.bus_reply(cid, data if data is not None else bytes(rx_len)))


def usage():
    lines = ["usage: cosim-tcp.py [--host HOST] [--name NAME] PORT DEVICE [options] [+ DEVICE [options] ...]",
             "       cosim-tcp.py PORT DEVICE --help", "",
             "Devices on esp-emu's co-simulation socket (--cosim-tcp). Several",
             "devices joined by '+' share the connection, as one board.", "",
             "  --host HOST   where esp-emu listens (default 127.0.0.1)",
             "  --name NAME   the consumer's name in the emulator's log", "",
             "devices:"]
    for name in sorted(DEVICES):
        doc = (DEVICES[name].__doc__ or "").strip().splitlines()
        lines.append(f"  {name:<10} {doc[0] if doc else ''}")
    return "\n".join(lines)


def main(argv=None):
    from . import devices  # noqa: F401  (imports every module, filling DEVICES)

    # --host and --name belong to the connection: taken wherever they stand.
    top = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    top.add_argument("--host", default="127.0.0.1")
    top.add_argument("--name")
    conn, argv = top.parse_known_args(argv)
    if len(argv) < 2 or not argv[0].isdigit():
        sys.exit(usage())
    port, groups = int(argv[0]), [[]]
    for word in argv[1:]:
        if word == "+":
            groups.append([])
        else:
            groups[-1].append(word)
    built = []
    for group in groups:
        if not group or group[0] not in DEVICES:
            sys.exit(f"no device {group[0] if group else ''!r}\n\n{usage()}")
        cls = DEVICES[group[0]]
        ap = argparse.ArgumentParser(prog=f"cosim-tcp.py PORT {cls.name}", description=cls.__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
        cls.arguments(ap)
        built.append(cls(ap.parse_args(group[1:])))
    failed = Session(port, built, conn.host, conn.name or "+".join(d.name for d in built)).run()
    if failed:
        sys.exit("\n".join(failed))
