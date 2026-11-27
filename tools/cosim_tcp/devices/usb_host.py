# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import struct

from .. import wire
from ..device import Device


class Stall(Exception):
    """A transfer did not end OK; `args[0]` is the status word."""


def setup(request_type, request, value=0, index=0, length=0):
    return struct.pack("<BBHHH", request_type, request, value, index, length)


def descriptors(config):
    """Each descriptor of a configuration descriptor, in order."""
    i = 0
    while i + 2 <= len(config) and config[i] >= 2:
        yield config[i:i + config[i]]
        i += config[i]


# HID usage page 7, for a boot keyboard's reports.
KEYS = {**{4 + i: chr(ord("a") + i) for i in range(26)},
        **{30 + i: c for i, c in enumerate("1234567890")},
        40: "Enter", 41: "Esc", 42: "Backspace", 43: "Tab", 44: "Space"}


class UsbHost(Device):
    """The computer a USB device firmware is plugged into: enumerates it and serves its interfaces.

    Takes a USB-OTG port from the emulator's own host, enumerates whatever
    the firmware presents, prints its descriptors, and then plays the host
    side of each interface it knows:

      mass storage   INQUIRY, READ CAPACITY, the first block; --msc-write FILE
                     writes a file onto the disk from block 0, and
                     --msc-dump FILE then reads the whole disk into one
      HID            every input report, a boot keyboard's as its keys
      CDC-ACM        opens the port and prints what the device writes

        esp-emu --chip esp32p4 --firmware msc.bin --sd card.img --cosim-tcp 127.0.0.1:5590 &
        python3 tools/cosim-tcp.py 5590 usb-host --msc-dump disk.img

    It leaves once every bounded job is done (the disk read; --reports N
    reports; --cdc-bytes N bytes), or never if there is none.
    """

    name = "usb-host"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--port", type=int, default=0, help="the chip's USB-OTG port (default %(default)s)")
        ap.add_argument("--msc-write", metavar="FILE", help="write FILE onto a mass-storage disk, from block 0")
        ap.add_argument("--msc-dump", metavar="FILE", help="read a mass-storage disk whole into FILE")
        ap.add_argument("--msc-blocks", type=int, default=0, help="read only this many blocks of it")
        ap.add_argument("--reports", type=int, default=0, help="leave after this many HID reports")
        ap.add_argument("--cdc-bytes", type=int, default=0, help="leave after this many CDC bytes")

    def claims(self):
        return [wire.usb_host_claim(self.args.port)]

    def attached(self, ack):
        self.ident = 0
        self.tag = 0                # of the last mass-storage command
        self.waiting = {}           # transfer id: the task waiting on it
        self.bounded = set()        # the tasks the run waits for

    # -- tasks: generators that yield a transfer and are sent its result ----

    def spawn(self, task, bounded=False):
        if bounded:
            self.bounded.add(task)
        self.resume(task, None)

    def resume(self, task, result):
        try:
            request = task.send(result) if result is None or result[0] == "ok" else task.throw(Stall(result[0]))
        except StopIteration:
            if task in self.bounded:
                self.bounded.remove(task)
                if not self.bounded:
                    self.finish()
            return
        except Stall as e:
            if task in self.bounded:
                self.fail(f"a transfer ended {e.args[0]}")
            else:
                print(f"usb: a transfer ended {e.args[0]}; that interface is left alone", flush=True)
            return
        self.ident += 1
        self.waiting[self.ident] = task
        self.bus.usb_transfer(self.ident, **request)

    def usb_result(self, ident, status, data, sent):
        task = self.waiting.pop(ident, None)
        if task is not None:
            self.resume(task, (status, data, sent))

    def usb_link(self, attached, speed):
        self.waiting.clear()        # what was under way went with the device,
        self.bounded.clear()        # to start over when it is enumerated again
        if attached:
            print(f"usb: device attached, {speed} speed", flush=True)
            self.spawn(self.enumerate(speed))
        else:
            print("usb: no device", flush=True)

    # -- transfers ----------------------------------------------------------

    @staticmethod
    def control(request_type, request, value=0, index=0, length=0, data=b""):
        length = len(data) if data else length
        return dict(endpoint=0, setup=setup(request_type, request, value, index, length), data=data,
                    timeout_us=1_000_000)

    @staticmethod
    def bulk(ep, **what):
        return dict(endpoint=ep["addr"], timeout_us=5_000_000, **what)

    def get_descriptor(self, kind, index, length, lang=0):
        _, data, _ = yield self.control(0x80, 6, kind << 8 | index, lang, length)
        return data

    def string(self, index, lang):
        if not index:
            return ""
        try:
            raw = yield from self.get_descriptor(3, index, 255, lang)
        except Stall:
            return ""
        return raw[2:].decode("utf-16-le", errors="replace")

    # -- enumeration --------------------------------------------------------

    def enumerate(self, speed):
        dev = yield from self.get_descriptor(1, 0, 18)
        yield self.control(0x00, 5, 1)                               # SET_ADDRESS
        vid, pid, bcd = struct.unpack_from("<HHH", dev, 8)
        try:
            langs = yield from self.get_descriptor(3, 0, 255)
            lang = struct.unpack_from("<H", langs, 2)[0]
        except Stall:
            lang = 0
        maker = yield from self.string(dev[14], lang)
        product = yield from self.string(dev[15], lang)
        print(f"usb: {vid:04x}:{pid:04x} {maker!r} {product!r}, class {dev[4]:#04x}, release {bcd:#06x}", flush=True)

        head = yield from self.get_descriptor(2, 0, 9)
        config = yield from self.get_descriptor(2, 0, struct.unpack_from("<H", head, 2)[0])
        interfaces, current = [], None
        for d in descriptors(config):
            if d[1] == 4:                                            # interface
                current = dict(number=d[2], cls=d[5], sub=d[6], proto=d[7], eps=[])
                interfaces.append(current)
                print(f"usb:   interface {d[2]}: class {d[5]:#04x} subclass {d[6]:#04x} protocol {d[7]:#04x}", flush=True)
            elif d[1] == 5 and current is not None:                  # endpoint
                mps = struct.unpack_from("<H", d, 4)[0] & 0x7FF
                current["eps"].append(dict(addr=d[2], kind=d[3] & 3, mps=mps))
                kind = ("control", "isochronous", "bulk", "interrupt")[d[3] & 3]
                print(f"usb:     endpoint {d[2]:#04x} {kind}, {mps} bytes", flush=True)
        yield self.control(0x00, 9, config[5])                       # SET_CONFIGURATION
        print(f"usb: configuration {config[5]} set", flush=True)

        for i in interfaces:
            ep = lambda kind, is_in: next(                           # noqa: E731
                (e for e in i["eps"] if e["kind"] == kind and bool(e["addr"] & 0x80) == is_in), None)
            if i["cls"] == 8 and i["proto"] == 0x50 and ep(2, True) and ep(2, False):
                bounded = bool(self.args.msc_dump or self.args.msc_write)
                self.spawn(self.mass_storage(i["number"], (ep(2, False), ep(2, True))), bounded)
            elif i["cls"] == 3 and ep(3, True):
                self.spawn(self.hid(i, ep(3, True)), bool(self.args.reports))
            elif i["cls"] == 0x0A and ep(2, True):
                self.spawn(self.cdc(i["number"], ep(2, True)), bool(self.args.cdc_bytes))

    # -- mass storage: bulk-only transport, SCSI ----------------------------

    def scsi(self, eps, cdb, length=0, write=b""):
        """One command with a data-in stage of `length` bytes, or a data-out
        stage of `write`: the data read."""
        out, inn = eps
        self.tag += 1
        flags, size = (0, len(write)) if write else (0x80, length)
        cbw = struct.pack("<IIIBBB", 0x43425355, self.tag, size, flags, 0, len(cdb)) + cdb.ljust(16, b"\0")
        yield self.bulk(out, data=cbw)
        data = b""
        if write:
            yield self.bulk(out, data=write)
        elif length:
            _, data, _ = yield self.bulk(inn, length=length)
        _, csw, _ = yield self.bulk(inn, length=13)
        sig, tag, _residue, status = struct.unpack("<IIIB", csw.ljust(13, b"\0")[:13])
        if len(csw) != 13 or sig != 0x53425355 or tag != self.tag:
            raise Stall(f"a bad status wrapper {csw.hex()}")
        if status:
            raise Stall(f"SCSI status {status} for command {cdb[0]:#04x}")
        return data

    def mass_storage(self, number, eps):
        _, luns, _ = yield self.control(0xA1, 0xFE, 0, number, 1)    # GET_MAX_LUN
        inq = yield from self.scsi(eps, bytes([0x12, 0, 0, 0, 36, 0]), 36)
        print(f"msc: {inq[8:16].decode(errors='replace').strip()} {inq[16:32].decode(errors='replace').strip()}"
              f", {luns[0] + 1} unit(s)", flush=True)
        for _ in range(20):                                          # TEST UNIT READY, while it spins up
            try:
                yield from self.scsi(eps, bytes(6))
                break
            except Stall:
                yield from self.scsi(eps, bytes([0x03, 0, 0, 0, 18, 0]), 18)   # REQUEST SENSE
        cap = yield from self.scsi(eps, bytes([0x25]) + bytes(9), 8)
        last, size = struct.unpack(">II", cap)
        print(f"msc: {last + 1} blocks of {size} bytes, {(last + 1) * size / 1e6:.1f} MB", flush=True)

        def read(lba, count):
            return self.scsi(eps, struct.pack(">BBIBHB", 0x28, 0, lba, 0, count, 0), count * size)

        first = yield from read(0, 1)
        boot = "a boot sector" if first[510:512] == b"\x55\xaa" else "no boot signature"
        print(f"msc: block 0: {first[:16].hex(' ')} ... ({boot})", flush=True)
        if not (self.args.msc_dump or self.args.msc_write):
            return
        chunk = max(1, 32768 // size)                                # blocks a command, well under a message
        if self.args.msc_write:
            with open(self.args.msc_write, "rb") as f:
                image = f.read()[:(last + 1) * size]
            image += bytes(-len(image) % size)
            for at in range(0, len(image), chunk * size):
                piece = image[at:at + chunk * size]
                cdb = struct.pack(">BBIBHB", 0x2A, 0, at // size, 0, len(piece) // size, 0)
                yield from self.scsi(eps, cdb, write=piece)
            print(f"msc: {len(image) // size} blocks written from {self.args.msc_write}", flush=True)
        if self.args.msc_dump:
            blocks = min(self.args.msc_blocks or last + 1, last + 1)
            with open(self.args.msc_dump, "wb") as f:
                lba = 0
                while lba < blocks:
                    n = min(chunk, blocks - lba)
                    f.write((yield from read(lba, n)))
                    lba += n
            print(f"msc: {blocks} blocks read into {self.args.msc_dump}", flush=True)

    # -- HID ------------------------------------------------------------------

    def hid(self, i, inn):
        bounded = bool(self.args.reports)
        try:
            yield self.control(0x21, 0x0A, 0, i["number"])            # SET_IDLE: report only changes
        except Stall:
            pass
        keyboard = (i["sub"], i["proto"]) == (1, 1)
        print(f"hid: interface {i['number']}{', a boot keyboard' if keyboard else ''}: waiting for reports", flush=True)
        seen = 0
        while not bounded or seen < self.args.reports:
            # No timeout: a host's interrupt transfer waits until there is a report.
            _, report, _ = yield dict(endpoint=inn["addr"], length=inn["mps"])
            seen += 1
            if keyboard and len(report) >= 8:
                keys = [KEYS.get(k, f"key {k:#04x}") for k in report[2:8] if k]
                mods = f" modifiers {report[0]:#04x}" if report[0] else ""
                print(f"hid: keys {keys or 'released'}{mods}", flush=True)
            else:
                print(f"hid: report {report.hex(' ')}", flush=True)

    # -- CDC-ACM --------------------------------------------------------------

    def cdc(self, number, inn):
        bounded = bool(self.args.cdc_bytes)
        coding = struct.pack("<IBBB", 115200, 0, 0, 8)
        try:
            # The class requests go to the communication interface, the one before.
            yield self.control(0x21, 0x20, 0, number - 1, data=coding)   # SET_LINE_CODING
            yield self.control(0x21, 0x22, 3, number - 1)                # DTR and RTS
        except Stall:
            pass
        got = 0
        while not bounded or got < self.args.cdc_bytes:
            _, data, _ = yield dict(endpoint=inn["addr"], length=inn["mps"])
            got += len(data)
            print(data.decode(errors="replace"), end="", flush=True)
