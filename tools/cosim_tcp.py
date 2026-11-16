# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""The co-simulation socket (--cosim-tcp) for a Python script, with no
protobuf library: the framing, the few messages of proto/esp_cosim.proto a
device needs, encoded and decoded by hand (docs/guides/HOST-INTERFACES.md,
"Co-simulation over TCP"). Imported by the scripts beside it.

A message is a dict of field number to value as `decode` gives it; the
field numbers are the .proto's. A field at its default (0, false, empty) is
not on the wire, so read one with `.get(number, default)`.
"""
import socket
import struct
import sys
import time


# ---------------------------------------------------------------- protobuf

def varint(n):
    out = b""
    while n > 0x7F:
        out += bytes([(n & 0x7F) | 0x80])
        n >>= 7
    return out + bytes([n])


def field(num, payload):          # a varint field
    return varint(num << 3) + payload


def ld(num, inner):               # a length-delimited field
    return varint((num << 3) | 2) + varint(len(inner)) + inner


def read_varint(buf, i):
    v, shift = 0, 0
    while True:
        b = buf[i]
        i += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return v, i


def decode(buf):
    """A message's fields as {number: value}, the last of a repeated one."""
    out, i = {}, 0
    while i < len(buf):
        tag, i = read_varint(buf, i)
        num, wire = tag >> 3, tag & 7
        if wire == 0:
            v, i = read_varint(buf, i)
        elif wire == 1:
            v, i = buf[i:i + 8], i + 8
        elif wire == 2:
            n, i = read_varint(buf, i)
            v, i = buf[i:i + n], i + n
        elif wire == 5:
            v, i = buf[i:i + 4], i + 4
        else:
            raise ValueError(f"wire type {wire}")
        out[num] = v
    return out


# ---------------------------------------------------------------- messages

def gpio_claim(drive_pins, watch_pins=0):
    return ld(1, field(1, varint(drive_pins)) + field(2, varint(watch_pins)))   # Claim.gpio


def spi_claim(cs_pin):
    return ld(3, field(1, varint(cs_pin)))                           # Claim.spi.cs_pin


def i2c_claim(addr, scl_pin, sda_pin):
    """The device at 7-bit `addr` on the pads the firmware gives its bus."""
    c = field(1, varint(addr)) + field(2, varint(scl_pin)) + field(3, varint(sda_pin))
    return ld(2, c)                                                  # Claim.i2c


def i2s_claim(dout_pin=None, din_pin=None):
    """The audio device on the pad the chip transmits on (`dout_pin`: a
    speaker hears I2sOut), the pad it receives on (`din_pin`: a microphone
    sends I2sIn), or both."""
    c = b""
    if dout_pin is not None:
        c += field(3, varint(dout_pin))
    if din_pin is not None:
        c += field(4, varint(din_pin))
    return ld(17, c)                                                 # Claim.i2s


def dvp_claim(pclk_pin, vsync_pin, data_pins, de_pin=None, hsync_pin=None, xclk_pin=None):
    """The camera sensor on a DVP bus: its pixel clock, VSYNC and the eight
    data pads, D0 first, as the firmware's pin config names them."""
    c = field(1, varint(pclk_pin)) + field(2, varint(vsync_pin))
    for num, pin in ((3, de_pin), (4, hsync_pin), (5, xclk_pin)):
        if pin is not None:
            c += field(num, varint(pin))
    c += b"".join(field(6, varint(pin)) for pin in data_pins)
    return ld(18, c)                                                 # Claim.dvp


def dsi_claim(virtual_channel=0):
    """The panel on a virtual channel of the MIPI-DSI link."""
    return ld(20, field(1, varint(virtual_channel)))                 # Claim.dsi


def en_claim():
    """Wired to the chip's EN (CHIP_PU) pin: may pull it low with en_drive."""
    return ld(19, b"")                                               # Claim.en


def register(name, *claims):
    """One consumer with its claims; a board of devices is one consumer."""
    reg = field(1, varint(1)) + ld(2, name.encode()) + b"".join(ld(3, c) for c in claims)
    return ld(4, reg)                                                # EmulatorMessage.register


def gpio_drive(at_ns, pin, level):
    g = field(1, varint(pin)) + field(2, varint(int(level)))
    return field(1, varint(at_ns)) + ld(6, g)                        # sim_time_ns, gpio_drive


def en_drive(at_ns, level):
    """EN at `at_ns`: False holds the chip in reset while emulated time goes
    on, True lets go and the chip boots as from power-on. Needs en_claim."""
    e = field(1, varint(1)) if level else b""
    return field(1, varint(at_ns)) + ld(34, e)                       # sim_time_ns, en_drive


def en_level(payload):
    """`(at_ns, level)` of an EnLevel broadcast (topic SIM/EN), else None."""
    msg = decode(payload)
    if 35 not in msg:
        return None
    return msg.get(1, 0), bool(decode(msg[35]).get(1, 0))


def bus_reply(correlation_id, data, ack=True, op=2):
    """The answer to a BusRequest: `data` is what the master reads. `op` is
    2 for SPI (the default), 1 for I2C; `ack=False` is an I2C NACK."""
    r = field(1, varint(correlation_id)) + field(2, varint(int(ack))) + ld(3, data) + field(4, varint(op))
    return ld(8, r)                                                  # EmulatorMessage.bus_reply


def i2c_reply(correlation_id, data=b"", ack=True):
    return bus_reply(correlation_id, data, ack, op=1)


class I2cRequest:
    """One I2C transaction the firmware sent: `write` is every byte it
    clocked out, `read_len` how many it reads back (0: write-only; both 0:
    a probe, ACK it if the device is there). A long read arrives in pieces:
    each after the first has `continues` set and no write bytes, and the
    device answers from where its read left off."""

    def __init__(self, t):
        self.bus = t.get(1, 0)
        self.addr = t.get(2, 0)
        self.write = t.get(3, b"")
        self.read_len = t.get(4, 0)
        self.continues = bool(t.get(5, 0))


def bus_request(payload):
    """A frame's payload as (correlation_id, BusRequest fields), or None when
    the frame is not a BusRequest (a heartbeat, a pin level, a reset)."""
    req = decode(payload).get(7)                                     # EmulatorMessage.bus_request
    if req is None:
        return None
    r = decode(req)
    return r.get(1, 0), r


def i2c_request(payload):
    """A frame's payload as (correlation_id, I2cRequest), or None when the
    frame is not an I2C BusRequest."""
    br = bus_request(payload)
    if br is None or 2 not in br[1]:                                 # BusRequest.i2c
        return None
    return br[0], I2cRequest(decode(br[1][2]))


def i2s_in(pcm):
    """Samples for the chip's receiver, in the receive I2sFormat's layout."""
    return ld(31, ld(1, pcm))                                        # EmulatorMessage.i2s_in


def i2s_format(payload):
    """A frame's payload as (receive, sample_rate_hz, channels, bits), or
    None when the frame is not an I2sFormat."""
    f = decode(payload).get(29)                                      # EmulatorMessage.i2s_format
    if f is None:
        return None
    f = decode(f)
    return bool(f.get(1, 0)), f.get(2, 0), f.get(3, 0), f.get(4, 0)


def i2s_out(payload):
    """The samples a frame carries from the chip's transmitter, or None when
    the frame is not an I2sOut."""
    o = decode(payload).get(30)                                      # EmulatorMessage.i2s_out
    return None if o is None else decode(o).get(1, b"")


def fourcc(name):
    """V4L2's four characters as CamFrame carries them: 'JPEG', 'RGBP'..."""
    return struct.unpack("<I", name.encode())[0]


def dvp_format(payload):
    """A frame's payload as a dict of the DvpFormat it carries (capturing,
    data_width, xclk_hz, the three invert flags and the session), or None."""
    f = decode(payload).get(32)                                      # EmulatorMessage.dvp_format
    if f is None:
        return None
    f = decode(f)
    return {"capturing": bool(f.get(1, 0)), "data_width": f.get(2, 0), "xclk_hz": f.get(3, 0),
            "pclk_invert": bool(f.get(4, 0)), "vsync_invert": bool(f.get(5, 0)), "de_invert": bool(f.get(6, 0)),
            "session": f.get(7, 0)}


def cam_frame_pieces(data, width, height, four, piece=128 * 1024):
    """One camera frame cut into pieces the transport takes and encoded,
    for `cam_frame` to send. A frame sent again and again is encoded once."""
    out = []
    for at in range(0, max(len(data), 1), piece):
        f = ld(1, data[at:at + piece])
        if at + piece < len(data):
            out.append(ld(33, f))                                    # EmulatorMessage.cam_frame
        else:                                                        # the last: what the frame is
            f += field(2, varint(1)) + field(3, varint(width)) + field(4, varint(height))
            out.append(f + bytes([(5 << 3) | 5]) + struct.pack("<I", four))   # fixed32 fourcc
    return out


def cam_frame(pieces, due_ns=0, session=0):
    """The messages of a frame from `cam_frame_pieces`, to start at `due_ns`
    of emulated time (0 lets the emulator pace it), for the capture session
    the last DvpFormat named."""
    stamp = field(1, varint(due_ns)) if due_ns else b""
    return pieces[:-1] + [stamp + ld(33, pieces[-1] + field(6, varint(session)))]


def dsi_event(payload):
    """What a frame's DsiEvent carries, or None when it is not one:
    ("command", dt, data), ("format", streaming, width, height, format) or
    ("rows", frame, y, rows, pixels)."""
    e = decode(payload).get(36)                                      # EmulatorMessage.dsi_event
    if e is None:
        return None
    e = decode(e)
    if 1 in e:
        c = decode(e[1])
        return "command", c.get(1, 0), c.get(2, b"")
    if 2 in e:
        f = decode(e[2])
        return "format", bool(f.get(1, 0)), f.get(2, 0), f.get(3, 0), f.get(4, 0)
    if 3 in e:
        r = decode(e[3])
        return "rows", r.get(1, 0), r.get(2, 0), r.get(3, 0), r.get(4, b"")
    return None


def accepted(ack):
    """Whether a RegisterAck took the registration, and its reason if not."""
    a = decode(decode(ack or b"").get(5, b""))
    return bool(a.get(5, 0)), a.get(6, b"").decode(errors="replace")


def en_low(ack):
    """Whether a RegisterAck says the chip's EN pin is low at attach."""
    return bool(decode(decode(ack or b"").get(5, b"")).get(8, 0))


# ---------------------------------------------------------------- the socket

def send(s, msg):
    s.sendall(struct.pack("<I", len(msg)) + msg)


def recv(s):
    """The next frame as (topic, payload); (None, None) once the emulator
    has left."""
    head = s.recv(4, socket.MSG_WAITALL)
    if len(head) < 4:
        return None, None
    (n,) = struct.unpack("<I", head)
    f = s.recv(n, socket.MSG_WAITALL)
    if len(f) < max(n, 2):
        return None, None
    (tl,) = struct.unpack("<H", f[:2])
    return f[2:2 + tl].decode(), f[2 + tl:]


def connect(port, host="127.0.0.1"):
    """Connect, waiting a few seconds for an emulator still starting."""
    for _ in range(50):
        try:
            return socket.create_connection((host, port))
        except ConnectionRefusedError:
            time.sleep(0.1)
    sys.exit("nothing listening: is esp-emu running with --cosim-tcp?")
