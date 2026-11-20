#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Renode's STM32F746 as the SPI master of an ESP chip in esp-emu.

Starts esp-emu with a firmware built from IDF's
examples/peripherals/spi_slave/receiver, then Renode with its stock STM32F746
platform and EspEmuSpiBridge.cs on SPI1, and exchanges a few 128-byte frames,
checking what each end read from the other.

    python3 tools/renode/spi_slave_demo.py \\
        --renode ~/renode_1.17.0-portable/renode \\
        --chip esp32c6 \\
        --firmware receiver/build/merged_flash.bin --quantum-us 100

The STM32 has no firmware here: this script plays it through Renode's
monitor, register by register (PA4 the chip select, PB0 the handshake input,
SPI1_DR the data register), so it needs no ARM toolchain. A real master
firmware does the same through the same models.

With --quantum-us the Renode machine holds the ESP's clock (TimeClaim): the
two advance one quantum at a time, each waiting for the other, and the
script reports both clocks at the end.
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GPIOA, GPIOB, SPI1 = 0x40020000, 0x40020400, 0x40013000
MODER, IDR, ODR, DR = 0x00, 0x10, 0x14, 0x0C


class Monitor:
    """Renode's monitor on its telnet port: one command, one answer."""

    PROMPT = re.compile(rb"\([^)\n]*\) $")

    def __init__(self, port):
        for _ in range(300):
            try:
                self.s = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(0.1)
        else:
            sys.exit("Renode's monitor did not open")
        self.read()

    def read(self, timeout=30):
        buf, end = b"", time.time() + timeout
        self.s.settimeout(1.0)
        while time.time() < end:
            try:
                got = self.s.recv(65536)
            except socket.timeout:
                continue
            if not got:
                break
            buf += got
            if self.PROMPT.search(buf.replace(b"\r", b"")):
                break
        return buf.decode(errors="replace")

    def cmd(self, line):
        self.s.sendall(line.encode() + b"\n")
        return self.read().split(line, 1)[-1]

    def write(self, addr, val):
        self.cmd(f"sysbus WriteDoubleWord 0x{addr:08X} 0x{val:X}")

    def read32(self, addr):
        found = re.findall(r"0x[0-9A-Fa-f]+", self.cmd(f"sysbus ReadDoubleWord 0x{addr:08X}"))
        if not found:
            raise RuntimeError(f"no value read at 0x{addr:08X}")
        return int(found[-1], 16)

    def number(self, line):
        found = re.findall(r"0x[0-9A-Fa-f]+|\b\d+\b", self.cmd(line))
        return int(found[-1], 0) if found else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--renode", required=True, help="the renode executable")
    ap.add_argument("--emu", default="esp-emu", help="the esp-emu executable: a release binary, or a source build with --features cosim")
    ap.add_argument("--firmware", required=True, help="merged flash image of IDF's spi_slave/receiver example")
    ap.add_argument("--chip", default="esp32c6")
    ap.add_argument("--cs", type=int, default=14, help="the example's GPIO_CS")
    ap.add_argument("--handshake", type=int, default=2, help="the example's GPIO_HANDSHAKE")
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--len", type=int, default=128, help="bytes a frame")
    ap.add_argument("--quantum-us", type=int, default=0, help="hold the ESP's clock, in steps of this much virtual time")
    ap.add_argument("--port", type=int, default=5590, help="esp-emu's --cosim-tcp port; Renode's monitor takes the next")
    ap.add_argument("--work", help="directory for the generated platform, script and logs (default: a temporary one)")
    ap.add_argument("--keep", action="store_true", help="keep the work directory with the logs")
    args = ap.parse_args()

    if args.work:
        work = args.work
        os.makedirs(work, exist_ok=True)
    else:
        work = tempfile.mkdtemp(prefix="esp-renode-")
    open(f"{work}/board.repl", "w").write(f"""using "platforms/cpus/stm32f746.repl"

esp: SPI.EspEmuSpiBridge @ spi1
    port: {args.port}
    csPin: {args.cs}
    handshakePin: {args.handshake}
    quantumUs: {args.quantum_us}

gpioPortA:
    4 -> syscfg#0@4 | esp@0

esp:
    Handshake -> gpioPortB@0
""")
    # The CPU is halted: the script is the firmware. Virtual time still runs.
    open(f"{work}/run.resc", "w").write(f"""mach create "stm32"
include @{HERE}/EspEmuSpiBridge.cs
machine LoadPlatformDescription @{work}/board.repl
cpu IsHalted true
start
""")

    emu = subprocess.Popen([args.emu, "--chip", args.chip, "--firmware", args.firmware,
                            "--cosim-tcp", f"127.0.0.1:{args.port}", "--timeout", "300s"],
                           stdout=open(f"{work}/esp.log", "w"), stderr=subprocess.STDOUT,
                           env=dict(os.environ, RUST_LOG=os.environ.get("RUST_LOG", "warn")))
    time.sleep(1.0)
    ren = subprocess.Popen([args.renode, "--disable-gui", "--plain", "-P", str(args.port + 1), f"{work}/run.resc"],
                           stdout=open(f"{work}/renode.log", "w"), stderr=subprocess.STDOUT)
    failed = None
    try:
        mon = Monitor(args.port + 1)
        for _ in range(150):
            if "esp" in mon.cmd("peripherals"):
                break
            time.sleep(0.2)
        else:
            raise RuntimeError("Renode did not build the machine; see renode.log")
        # PA4 a push-pull output, high: the slave deselected.
        mon.write(GPIOA + MODER, 0xA8000100)
        mon.write(GPIOA + ODR, 1 << 4)
        last = b""
        for n in range(args.frames):
            end = time.time() + 60
            while not mon.read32(GPIOB + IDR) & 1:
                if time.time() > end:
                    raise RuntimeError("the handshake line never rose")
                time.sleep(0.01)
            text = f'Sender, transmission no. {n:04d}. Last time, I received: "{last.decode(errors="replace")}"'
            tx = text.encode()[:args.len - 1].ljust(args.len, b"\0")
            mon.write(GPIOA + ODR, 0)
            rx = bytearray()
            for b in tx:
                mon.write(SPI1 + DR, b)
                rx.append(mon.read32(SPI1 + DR) & 0xFF)
            mon.write(GPIOA + ODR, 1 << 4)
            got = bytes(rx).split(b"\0")[0]
            want = f"This is the receiver, sending data for transmission number {n:04d}.".encode()[:args.len - 1]
            print(f"frame {n}: the STM32 read {got[:70]!r}", flush=True)
            if got != want:
                raise RuntimeError(f"frame {n}: expected {want!r}")
            last = got[:40]
            end = time.time() + 30
            while mon.read32(GPIOB + IDR) & 1 and time.time() < end:
                time.sleep(0.01)
        if args.quantum_us:
            mon.cmd("pause")
            time.sleep(0.2)
            quanta = mon.number("sysbus.spi1.esp QuantaGranted")
            ran_ns = mon.number("sysbus.spi1.esp EspTimeNs") - mon.number("sysbus.spi1.esp EspTimeAtAttachNs")
            granted_ns = quanta * args.quantum_us * 1000
            print(f"lockstep: {quanta} quanta of {args.quantum_us} us granted = {granted_ns / 1e9:.6f} s; "
                  f"the ESP ran {ran_ns / 1e9:.6f} s since Renode attached")
            # The ESP's time is as last heard: at most a heartbeat and a quantum behind.
            if not granted_ns - 2_000_000 - 2000 * args.quantum_us <= ran_ns <= granted_ns + 10_000:
                raise RuntimeError("the two clocks are not in step")
    except Exception as e:                                   # noqa: BLE001
        failed = f"{type(e).__name__}: {e}"
    finally:
        for p in (ren, emu):
            p.terminate()
        for p in (ren, emu):
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()

    esp_log = open(f"{work}/esp.log", errors="replace").read()
    received = [l for l in esp_log.splitlines() if l.startswith("Received: Sender")]
    for line in received:
        print("the ESP printed:", line[:110])
    if failed is None and len(received) < args.frames:
        failed = f"the ESP printed {len(received)} of {args.frames} frames"
    if failed or args.keep or args.work:
        print(f"logs kept in {work}")
    else:
        shutil.rmtree(work)
    if failed:
        sys.exit(failed)
    print("ok")


if __name__ == "__main__":
    main()
