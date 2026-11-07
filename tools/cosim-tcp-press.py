#!/usr/bin/env python3
"""A device on the co-simulation bus over --cosim-tcp, with no protobuf library.

Registers as the model driving GPIO 9, waits for a heartbeat, and presses the
pin for 300 ms of emulated time:

    esp-emu --chip esp32c3 --firmware app.bin --cosim-tcp 127.0.0.1:5590 &
    python3 tools/cosim-tcp-press.py 5590

The framing and the messages are in cosim_tcp.py beside this script.
"""
import sys
import time

from cosim_tcp import accepted, connect, decode, gpio_claim, gpio_drive, recv, register, send

PIN = 9
PRESS_NS = 300_000_000

s = connect(int(sys.argv[1]))
send(s, register("press.py", gpio_claim(1 << PIN)))
topic, ack = recv(s)
ok, reason = accepted(ack)
if not ok:
    sys.exit(f"refused on {topic}: {reason or ack!r}")
while True:
    topic, payload = recv(s)
    if topic is None:
        sys.exit("the emulator left before its first heartbeat")
    if topic == "SIM/CLOCK":
        now = decode(payload).get(1, 0)              # EmulatorMessage.sim_time_ns
        break
send(s, gpio_drive(now, PIN, False))
send(s, gpio_drive(now + PRESS_NS, PIN, True))
print(f"pressed GPIO {PIN} at {now / 1e6:.1f} ms for {PRESS_NS / 1e6:.0f} ms")
time.sleep(1)                                    # let the release land before leaving
