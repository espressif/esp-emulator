# esp-emu

A Rust-based RISC-V emulator that runs ESP32-C3, ESP32-C6, ESP32-P4, and ESP32-S31 firmware binaries — CPU, memory, WiFi, BLE, Thread, Ethernet, crypto, and more. Source lives in a private Espressif repository; this repo hosts the public installer, prebuilt release tarballs, user-facing docs, and helper tools.

## Install

One-liner (Linux x86_64 / macOS Apple Silicon):

```sh
curl -fsSL https://raw.githubusercontent.com/mahavirj/esp-emulator/main/install.sh | sh
```

This downloads the latest binary to `$HOME/.local/bin/esp-emu`. If `~/.local/bin` is not on your `PATH`, the installer prints the `export PATH=...` line to add.

Pin a specific version:

```sh
curl -fsSL https://raw.githubusercontent.com/mahavirj/esp-emulator/main/install.sh | sh -s -- --version 0.29.0
```

Other options: `--check` (print latest, no install), `--bin-dir DIR`, `--force`, `--quiet`. Full help:

```sh
curl -fsSL https://raw.githubusercontent.com/mahavirj/esp-emulator/main/install.sh | sh -s -- --help
```

## Updating

After install:

```sh
esp-emu update           # refresh in place
esp-emu update --check   # only print latest version
esp-emu --version        # print currently installed version
```

`esp-emu update` re-runs the installer against the directory holding the running binary, so it updates wherever you first installed it.

## Releases

Binary tarballs (single-file artifacts containing only the `esp-emu` executable) are published as [GitHub Releases](https://github.com/mahavirj/esp-emulator/releases). Each release ships three assets:

- `esp-emu-<version>-x86_64-unknown-linux-gnu.tar.gz` — Linux x86_64 native
- `esp-emu-<version>-aarch64-apple-darwin.tar.gz` — macOS Apple Silicon native
- `esp-emu-<version>-wasm.tar.gz` — browser WebAssembly bundle
- `SHA256SUMS` — sha256 for each tarball; verified automatically by `install.sh`

## Docs

User guides live at [`docs/guides/`](docs/guides/) and track the latest release:

- [`BROWSER.md`](docs/guides/BROWSER.md) — running the WASM build in a browser
- [`MATTER.md`](docs/guides/MATTER.md) — Matter / Thread testing
- [`HOSTED.md`](docs/guides/HOSTED.md) — ESP-Hosted P4↔C6 setup
- [`THREAD.md`](docs/guides/THREAD.md) — 802.15.4 / OpenThread
- [`RAINMAKER.md`](docs/guides/RAINMAKER.md) — RainMaker provisioning flow

## Helper tools

Companion scripts live at [`tools/`](tools/):

- `setup-tap.sh` — create the `tap0` device for TAP networking on Linux
- `bumble_test.py` — virtual BLE controller (Google Bumble) over TCP for HCI testing
- `ws-net-proxy.py` / `ws-net-proxy.rs` — WebSocket↔raw-Ethernet bridge for the browser build
- `vhci_bridge.py` — Linux VHCI HCI bridge

## Manual install (without install.sh)

```sh
# Download the asset for your platform from
# https://github.com/mahavirj/esp-emulator/releases/latest
tar -xzf esp-emu-<version>-<platform>.tar.gz
cp esp-emu-<version>-<platform>/esp-emu ~/.local/bin/
~/.local/bin/esp-emu --help
```
