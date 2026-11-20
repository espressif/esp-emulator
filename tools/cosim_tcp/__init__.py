# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Devices on esp-emu's co-simulation socket (--cosim-tcp), in Python with
no protobuf library.

- `wire`: the framing and the messages, as functions; re-exported here, so
  `from cosim_tcp import connect, register, i2c_claim` is a script's start.
- `device`: `Device`, a device as a class of event methods, and `Session`,
  the loop that runs several on one connection.
- `devices/`: the devices `tools/cosim-tcp.py` offers, one class each.
- `picture`: a frame buffer written to a PNG as it changes.
"""
from .wire import *  # noqa: F401,F403
from .device import DEVICES, Device, Session, main  # noqa: F401
