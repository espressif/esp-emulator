"""The co-simulation socket (--cosim-tcp) for a Python script, with no
protobuf library: the framing, the few messages of proto/esp_cosim.proto a
device needs, encoded and decoded by hand (docs/guides/HOST-INTERFACES.md,
"Co-simulation over TCP"). Imported by the scripts beside it.

A message is a dict of field number to value as `decode` gives it; the
field numbers are the .proto's.
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


def register(name, claim):
    reg = field(1, varint(1)) + ld(2, name.encode()) + ld(3, claim)  # proto_version, name, claims
    return ld(4, reg)                                                # EmulatorMessage.register


def gpio_drive(at_ns, pin, level):
    g = field(1, varint(pin)) + field(2, varint(int(level)))
    return field(1, varint(at_ns)) + ld(6, g)                        # sim_time_ns, gpio_drive


def bus_reply(correlation_id, data):
    r = field(1, varint(correlation_id)) + field(2, varint(1)) + ld(3, data) + field(4, varint(2))
    return ld(8, r)                                                  # EmulatorMessage.bus_reply, op SPI


def accepted(ack):
    """Whether a RegisterAck took the registration, and its reason if not."""
    a = decode(decode(ack or b"").get(5, b""))
    return bool(a.get(5, 0)), a.get(6, b"").decode(errors="replace")


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
