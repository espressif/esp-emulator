#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

#
# Pre-encrypt an ESP-IDF build for esp-emu and write the eFuse file that goes
# with it, so the chip boots with flash encryption already on instead of
# running the bootloader's first-boot key generation and in-place encryption.
#
# Usage:
#   ./tools/make-encrypted-flash.py --chip esp32c3 --build-dir build -o out
#   esp-emu --chip esp32c3 --firmware out/flash_enc.bin --efuse out/efuse.bin \
#       --elf build/app.elf
#
# It reads build/flasher_args.json for what goes where and the partition table
# for which partitions the bootloader keeps encrypted, runs `espsecure encrypt-flash-data` on each
# (so an ESP-IDF environment, or an installed esptool, must be on the PATH),
# and writes:
#   out/flash_enc.bin   the flash image, padded with 0xFF to --flash-size
#   out/efuse.bin       the key, its purpose and an odd flash-encryption count
#   out/flash_enc_key.bin  the key, generated unless --key names one
#
# The key file is espsecure's and espefuse's: an eFuse key block holds it
# byte-reversed. --aes256 takes a 64-byte XTS-AES-256 key in two key blocks.
# ESP32 uses its own cipher (AES-256 keyed by each block's offset) and one key
# block, BLK1.

import argparse
import importlib.util
import json
import secrets
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# eFuse facts per chip, from components/efuse/<chip>/esp_efuse_table.csv:
#   crypt_cnt:   BLK0 bit of SPI_BOOT_CRYPT_CNT
#   purpose:     (BLK0 bit of KEY_PURPOSE_0, field width, key slots)
#   aes256:      whether the chip has XTS_AES_256_KEY_1 / _2
# A BLK0 bit counts from EFUSE_RD_WR_DIS, which is word 0 of the file.
XTS_AES_256_KEY_1, XTS_AES_256_KEY_2, XTS_AES_128_KEY = 2, 3, 4
CHIPS = {
    "esp32c3": {"crypt_cnt": 82, "purpose": (88, 4, 6), "aes256": False},
    "esp32c5": {"crypt_cnt": 80, "purpose": (86, 5, 6), "aes256": False},
    "esp32c6": {"crypt_cnt": 82, "purpose": (88, 4, 6), "aes256": False},
    "esp32h2": {"crypt_cnt": 82, "purpose": (88, 4, 6), "aes256": False},
    "esp32p4": {"crypt_cnt": 82, "purpose": (88, 4, 6), "aes256": True},
    "esp32s3": {"crypt_cnt": 82, "purpose": (88, 4, 6), "aes256": True},
    "esp32s31": {"crypt_cnt": 85, "purpose": (96, 5, 5), "aes256": True},
}
# Words in a key block. The file's first one, KEY0, follows BLK3; see
# make-efuse.py.
KEY_WORDS = 8


def load_make_efuse():
    """tools/make-efuse.py: the blank-chip eFuse image this builds on."""
    path = Path(__file__).with_name("make-efuse.py")
    spec = importlib.util.spec_from_file_location("make_efuse", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def key_words(key):
    """A 32-byte key as the eFuse block's words: byte-reversed."""
    return list(struct.unpack("<8I", key[::-1]))


def efuse_esp32(base, key, mac):
    """ESP32: the key in BLK1, FLASH_CRYPT_CNT bit 0, FLASH_CRYPT_CONFIG 0xF."""
    words = list(struct.unpack("<31I", base.esp32_image(3, 0, mac)))
    words[0] |= 1 << 20
    words[5] |= 0xF << 28
    words[7:15] = key_words(key)
    return struct.pack("<31I", *words)


def efuse_xts(base, chip, key, slots, mac):
    """The newer chips: each half of the key in a key block with its purpose."""
    spec = CHIPS[chip]
    words = base.blank_words(chip, mac)
    base.put(words, [(0, spec["crypt_cnt"], 3)], 1)
    key0 = base.BLK_WORD_BASE[3] + KEY_WORDS
    bit0, width, _ = spec["purpose"]
    purposes = [XTS_AES_128_KEY] if len(key) == 32 else [XTS_AES_256_KEY_1, XTS_AES_256_KEY_2]
    for n, (slot, purpose) in enumerate(zip(slots, purposes)):
        base.put(words, [(0, bit0 + slot * width, width)], purpose)
        at = key0 + slot * KEY_WORDS
        words[at:at + KEY_WORDS] = key_words(key[32 * n:32 * n + 32])
    return struct.pack(f"<{base.NWORDS}I", *words)


def encrypted_ranges(table):
    """[(offset, size)] of the partitions the bootloader keeps encrypted.

    From a partition-table.bin: every app, and of the data partitions
    otadata, nvs_keys and those carrying the `encrypted` flag.
    """
    ranges = []
    for at in range(0, len(table) - 31, 32):
        magic, ptype, subtype, offset, size, _label, flags = struct.unpack_from("<HBBII16sI", table, at)
        if magic != 0x50AA:
            break
        if ptype == 0 or flags & 1 or (ptype == 1 and subtype in (0x00, 0x04)):
            ranges.append((offset, size))
    return ranges


def flash_layout(build_dir):
    """[(offset, path, encrypted)] from flasher_args.json.

    The bootloader and the partition table are encrypted, and any other file
    if the partition it lands in is one the bootloader keeps encrypted: a
    SPIFFS or NVS image in a plain data partition stays as it is. A build
    that does not flash its bootloader (Secure Boot) still names it.
    """
    args = json.loads((build_dir / "flasher_args.json").read_text())
    files = {int(off, 16): file for off, file in args.get("flash_files", {}).items()}
    always = set()
    for name in ("bootloader", "partition-table"):
        section = args.get(name)
        if section and "offset" in section:
            files.setdefault(int(section["offset"], 16), section["file"])
            always.add(int(section["offset"], 16))
    table = args.get("partition-table", {}).get("file")
    ranges = encrypted_ranges((build_dir / table).read_bytes()) if table else []

    def encrypted(offset):
        return offset in always or any(start <= offset < start + size for start, size in ranges)

    return [(off, build_dir / file, encrypted(off)) for off, file in sorted(files.items())]


def parse_size(text):
    t = text.strip().lower()
    for suffix, mult in (("mb", 1 << 20), ("m", 1 << 20), ("kb", 1 << 10), ("k", 1 << 10)):
        if t.endswith(suffix):
            return int(t[: -len(suffix)]) * mult
    return int(t, 0)


def main():
    base = load_make_efuse()
    p = argparse.ArgumentParser(description="Pre-encrypt an ESP-IDF build and write its eFuse file for esp-emu.")
    p.add_argument("--chip", required=True, choices=sorted([*CHIPS, "esp32"]))
    p.add_argument("--build-dir", type=Path, required=True, help="the IDF build directory (flasher_args.json)")
    p.add_argument("-o", "--out-dir", type=Path, required=True)
    p.add_argument("--key", type=Path, help="key file, as espsecure takes it (generated if absent)")
    p.add_argument("--aes256", action="store_true", help="XTS-AES-256: a 64-byte key in two key blocks")
    p.add_argument("--key-slot", type=int, default=0, help="eFuse key block for the key (default 0)")
    p.add_argument("--key-slot2", type=int, help="--aes256: key block for the second half (default: the next)")
    p.add_argument("--flash-size", type=parse_size, default="4M", help="size of the image (default 4M)")
    p.add_argument("--mac", default="24:0A:C4:00:00:01", type=base.parse_mac, help="factory MAC")
    a = p.parse_args()
    mac = a.mac
    if a.chip == "esp32":
        if a.aes256 or a.key_slot or a.key_slot2 is not None:
            p.error("esp32 has one flash encryption key, in BLK1")
        slots = []
    else:
        spec = CHIPS[a.chip]
        if a.aes256 and not spec["aes256"]:
            p.error(f"{a.chip} has no XTS-AES-256")
        slots = [a.key_slot] + ([a.key_slot + 1 if a.key_slot2 is None else a.key_slot2] if a.aes256 else [])
        if len(set(slots)) != len(slots) or not all(0 <= s < spec["purpose"][2] for s in slots):
            p.error(f"{a.chip} has key blocks 0-{spec['purpose'][2] - 1}, and the two halves need one each")
    key_len = 64 if a.aes256 else 32

    if not (a.build_dir / "flasher_args.json").exists():
        p.error(f"{a.build_dir}/flasher_args.json not found; run `idf.py build` first")
    a.out_dir.mkdir(parents=True, exist_ok=True)
    key_path = a.key or a.out_dir / "flash_enc_key.bin"
    if a.key and not a.key.exists():
        p.error(f"{a.key} not found; leave --key out to have a key generated")
    if not key_path.exists():
        key_path.write_bytes(secrets.token_bytes(key_len))
        print(f"generated a {key_len * 8}-bit key: {key_path}")
    key = key_path.read_bytes()
    if len(key) != key_len:
        p.error(f"{key_path} is {len(key)} bytes; the key is {key_len}")

    image = bytearray(b"\xff" * a.flash_size)
    scratch = tempfile.TemporaryDirectory()
    for offset, src, encrypted in flash_layout(a.build_dir):
        data = src.read_bytes()
        if encrypted:
            # The ciphers work on 16-byte blocks.
            padded = Path(scratch.name) / "plain.bin"
            padded.write_bytes(data + b"\xff" * (-len(data) % 16))
            dst = Path(scratch.name) / "enc.bin"
            cmd = [sys.executable, "-m", "espsecure", "encrypt-flash-data", "--keyfile", str(key_path),
                   "--address", hex(offset), "--output", str(dst), str(padded)]
            if a.chip != "esp32":
                cmd.insert(4, "--aes-xts")
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            data = dst.read_bytes()
        if offset + len(data) > len(image):
            p.error(f"{src.name} at {offset:#x} does not fit a {a.flash_size}-byte flash; raise --flash-size")
        image[offset:offset + len(data)] = data
        print(f"  {offset:#08x}  {src.name}{'' if encrypted else '  (plaintext)'}")

    flash = a.out_dir / "flash_enc.bin"
    flash.write_bytes(image)
    efuse = a.out_dir / "efuse.bin"
    efuse.write_bytes(efuse_esp32(base, key, mac) if a.chip == "esp32" else efuse_xts(base, a.chip, key, slots, mac))
    print(f"{flash}: {len(image)} bytes\n{efuse}: flash encryption on, key in "
          + ("BLK1" if a.chip == "esp32" else " and ".join(f"KEY{s}" for s in slots)))
    print(f"\nesp-emu --chip {a.chip} --firmware {flash} --efuse {efuse}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
