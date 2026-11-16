#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Hold the chip in reset through its EN pin over --cosim-tcp, with no
protobuf library.

Registers as a device wired to EN (CHIP_PU), waits for a heartbeat, pulls EN
low and lets it go 200 ms of emulated time later, or the time given (longer if
the emulator, which does not wait for a socket, has run past that by the time
the release arrives). The chip runs nothing meanwhile and then boots as from
power-on:

    esp-emu --chip esp32c3 --firmware app.bin --cosim-tcp 127.0.0.1:5590 &
    python3 tools/cosim-tcp-reset.py 5590 [HOLD_MS]

The framing and the messages are in cosim_tcp.py beside this script.
"""
import sys

from cosim_tcp import accepted, connect, decode, en_claim, en_drive, en_level, en_low, recv, register, send

hold_ns = int(float(sys.argv[2]) * 1e6) if len(sys.argv) > 2 else 200_000_000

s = connect(int(sys.argv[1]))
send(s, register("reset.py", en_claim()))
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
# The fall may land later than `now`: the emulator did not wait for this
# script. So the release is timed from the fall it reports.
send(s, en_drive(now, False))
fell = None
# Already low, by another holder: nothing falls, so there is no report to
# time from. This script's hold then runs from now.
if en_low(ack):
    print("EN is already low")
    fell = now
    send(s, en_drive(fell + hold_ns, True))
while True:
    topic, payload = recv(s)
    if topic is None:
        sys.exit("the emulator left while EN was low")
    if topic == "SIM/EN":
        at, level = en_level(payload)
        print(f"EN {'high' if level else 'low'} at {at / 1e6:.1f} ms")
        if not level and fell is None:
            fell = at
            send(s, en_drive(fell + hold_ns, True))
    elif topic == "SIM/RESET":
        # Leaving lets go of EN, so the script stays until here.
        print("the chip reset and boots as from power-on")
        break
