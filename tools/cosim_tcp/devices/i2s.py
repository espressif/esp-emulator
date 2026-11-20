# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import math
import struct
import sys
import wave

from .. import wire
from ..device import Device

LEAD_NS = 50_000_000
TONE_HZ = 440


def tone(first, frames, rate, channels, bits):
    """`frames` frames of the tone from frame `first`, every slot alike."""
    width = bits // 8
    peak = (1 << (bits - 1)) // 4
    fmt = {1: "<b", 2: "<h", 4: "<i"}[width]
    return b"".join(
        struct.pack(fmt, int(peak * math.sin(2 * math.pi * TONE_HZ * n / rate))) * channels
        for n in range(first, first + frames))


class I2sCodec(Device):
    """An audio codec: records what the chip plays, plays it a tone.

    A speaker that records what the chip plays to a WAV file, and a
    microphone that plays it a tone:

        esp-emu --chip esp32s31 --firmware app.bin --cosim-tcp 127.0.0.1:5594 &
        python3 tools/cosim-tcp.py 5594 i2s --dout 52 --din 40 --wav played.wav

    `--dout` is the pad the chip transmits on and `--din` the pad it
    receives on, as the firmware's `i2s_std_gpio_config_t` names them; give
    either or both. The microphone waits for the receive format, then keeps
    50 ms of a 440 Hz tone queued ahead of emulated time, in the layout the
    firmware programmed.
    """

    name = "i2s"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--dout", type=int, help="the pad the chip transmits on")
        ap.add_argument("--din", type=int, help="the pad the chip receives on")
        ap.add_argument("--wav", default="i2s-out.wav", help="where the speaker records (default %(default)s)")

    def __init__(self, args):
        super().__init__(args)
        if args.dout is None and args.din is None:
            sys.exit("i2s: name --dout, --din or both")
        self.out = None         # the WAV being recorded, opened at the transmit format
        self.heard = 0
        self.rx = None          # (rate, channels, bits) the receiver wants
        self.rx_since = 0       # emulated time the receive format arrived
        self.sent = 0           # frames supplied since then

    def claims(self):
        return [wire.i2s_claim(self.args.dout, self.args.din)]

    def i2s_format(self, receive, rate, channels, bits):
        print(f"{'receive' if receive else 'transmit'} format: {rate} Hz, {channels} slots, {bits} bits", flush=True)
        if receive:
            self.rx, self.rx_since, self.sent = (rate, channels, bits), self.bus.now, 0
        elif self.out is None:
            self.out = wave.open(self.args.wav, "wb")
            self.out.setnchannels(channels)
            self.out.setsampwidth(bits // 8)
            self.out.setframerate(rate)

    def i2s_out(self, pcm):
        if pcm and self.out is not None:
            self.out.writeframes(pcm)
            self.heard += len(pcm)

    def tick(self, now):
        if self.rx is None or now < self.rx_since:
            return
        rate, channels, bits = self.rx
        due = (now - self.rx_since + LEAD_NS) * rate // 1_000_000_000
        if due > self.sent:
            self.bus.send(wire.i2s_in(tone(self.sent, due - self.sent, rate, channels, bits)))
            self.sent = due

    def closed(self):
        if self.out is not None:
            self.out.close()
            print(f"recorded {self.heard} bytes to {self.args.wav}", flush=True)
        if self.rx is not None:
            print(f"supplied {self.sent} frames of a {TONE_HZ} Hz tone", flush=True)
