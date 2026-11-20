#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Devices on esp-emu's co-simulation socket (--cosim-tcp):

    esp-emu --chip esp32c3 --firmware app.bin --cosim-tcp 127.0.0.1:5590 &
    python3 tools/cosim-tcp.py 5590 press --pin 9

Run with no arguments for the list of devices, and `PORT DEVICE --help` for
one device's options. The devices are the classes in cosim_tcp/devices/.
"""
from cosim_tcp import main

main()
