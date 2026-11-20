# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""The devices of `tools/cosim-tcp.py`. Each module here holds `Device`
subclasses with a `name`; importing the package imports them all, so a new
device is one new file, or one new class in the file of its bus."""
import importlib
import os

for _f in sorted(os.listdir(__path__[0])):
    if _f.endswith(".py") and not _f.startswith("_"):
        importlib.import_module(f"{__name__}.{_f[:-3]}")
