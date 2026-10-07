# Browser Emulation Guide

Run ESP32-C3/C5/C6/H2/P4/S3/S31 firmware in a web browser using the WASM build of ESP-EMU. Everything runs inside the tab: the firmware image never leaves your machine.

## Hosted page

Every release is deployed to GitHub Pages at **<https://espressif.github.io/esp-emulator/>** by the public mirror's `publish-pages` workflow. It is the same `www/` bundle as the release's wasm tarball, served as static files; nothing runs server-side. The networking section is disabled there (see Limitations). Use it when you just want to run a firmware image without installing anything; build locally when you need networking or an unreleased change.

## Quick Start

### 1. Build the WASM package

```sh
# Install wasm-pack (one-time)
cargo install wasm-pack

# Build WASM package into www/pkg/
wasm-pack build --target web --out-dir www/pkg --no-typescript -- --features wasm --no-default-features
```

### 2. Serve the web UI

```sh
cd www
python3 -m http.server 8080
```

Open `http://localhost:8080` in Chrome/Firefox/Safari.

### 3. Load and run firmware

1. Select the target chip — the ROM button labels itself with the embedded default (e.g. `esp32c3 rev3 (embedded)`) and updates live when you change the chip. Click it to upload a custom ROM ELF as an override; the label switches to `Custom: <filename>`.
2. Set the **WiFi SSID** and **Password** fields to match your firmware's WiFi config
3. Drop a merged flash binary (`.bin`) on the **Firmware** box or the terminal, or click the box to pick one
4. Click **Run**

UART output appears in the terminal; typing in the terminal goes to the firmware's UART0. The firmware will connect to the emulated WiFi AP and get an IP address via DHCP.

### The dashboard

The right-hand panel shows the machine live while it runs (refreshed a few times a second) and after every **Step**:

- **Performance** — instruction rate, emulated time (from the chip's CPU clock), how fast emulated time advances against the wall clock right now (about 1× while the firmware idles, since the browser build is paced to the host clock and `vTaskDelay` takes real time; below 1× in compute-bound code), total cycles, and a 30-second sparkline of the instruction rate. Hover the chart for a reading.
- **Cores** — the program counter of each core. On the dual-core chips (P4, S3, S31) the second core shows as *held in reset* until the firmware releases it.
- **GPIO** — one square per pad the chip has. A pad the firmware configured as an output is drawn in orange, filled when driven high. The others are inputs, high by default (pull-up); click one to drive it low or high, which the firmware sees through GPIO reads and interrupts.
- **Registers** — the register file of core 0. RISC-V chips show `x0`–`x31` with their ABI names; the ESP32-S3 shows the Xtensa register window `a0`–`a15` plus `PS`, `SAR`, `WINDOWBASE`, `WINDOWSTART`, `EXCCAUSE`, `EXCVADDR`, `EPC1` and the loop registers. A value that changed since the previous refresh flashes orange.
- **Memory** — a hex dump of any address (64 B to 1 KiB). Tick **Refresh while running** to watch a buffer change live. Unmapped bytes read as `00`.

## Browser Networking

By default, the browser emulator runs in internal-only mode: WiFi association and DHCP work, but the firmware has no path to the real network. To enable real connectivity, use the WebSocket-to-TAP proxy.

### Architecture

```
Browser (WASM emulator)
  ↕ WebSocket (binary Ethernet frames)
ws-net-proxy.py
  ↕ read/write
TAP interface (tap0)
  ↕ IP forwarding + NAT
Host network / Internet
```

### Setup

#### 1. Create TAP interface with NAT

```sh
sudo ./tools/setup-tap.sh
```

This creates `tap0` with IP `192.168.4.1/24`, enables IP forwarding, and adds iptables NAT rules. Run `sudo ./tools/setup-tap.sh --teardown` to clean up.

#### 2. Start the WebSocket proxy

Run the Python proxy (requires `pip install websockets`):

```sh
python3 tools/ws-net-proxy.py
```

It listens on `ws://localhost:8765` by default. Use `--port` to change.

#### 3. Connect from the browser

1. In the web UI, the **Network** field shows `ws://localhost:8765`
2. Click **Connect Net** — the button turns red and shows "Disconnect" when connected
3. Click **Run** — the firmware now has real network access through the TAP interface

### Proxy options

| Flag | Default | Description |
|------|---------|-------------|
| `--tap NAME` | `tap0` | TAP interface name |
| `--port PORT` | `8765` | WebSocket listen port |
| `--host ADDR` | `0.0.0.0` (Rust) / `localhost` (Python) | Bind address |

### Verifying connectivity

After the firmware gets an IP (`192.168.4.2`), you can verify the network path:

```sh
# From the host, ping the emulated device
ping 192.168.4.2

# Watch traffic on the TAP interface
sudo tcpdump -i tap0 -n
```

## WiFi Configuration

The web UI has SSID and Password fields in the toolbar:

| Field | Default | Description |
|-------|---------|-------------|
| WiFi SSID | `myssid` | Must match the SSID configured in your firmware |
| Password | `mypassword` | WPA2-PSK passphrase (8-63 chars). Leave empty for Open mode |

These are applied when firmware is loaded. To change them, reload the firmware.

## Limitations

- **No TAP in browser** — Real networking requires the WebSocket proxy running on the host
- **Performance** — WASM runs ~2-5x slower than native. TCP-heavy workloads may experience higher latency
- **Pacing** — Emulated time is held to the host clock: when firmware idles (WFI, `vTaskDelay`) the worker waits the difference in `setTimeout` instead of running ahead, so delays and timeouts take real time as on a board. With the network attached each wait is capped at 5 ms so incoming frames are not left queued
- **Single connection** — The WebSocket proxy supports one browser tab at a time per TAP interface
- **Linux only** — TAP networking and the proxy require Linux. The emulator itself (without networking) runs in any modern browser
- **HTTPS pages have no networking** — a page served over HTTPS (such as a hosted copy) cannot open the `ws://` link to a local proxy, so the *Network proxy* section is disabled there. Serve the page from `http://localhost` to use it

## Troubleshooting

### "WASM module loaded" never appears
- Make sure you're serving via HTTP (`python3 -m http.server`), not opening `index.html` directly (`file://` URLs block WASM imports)

### Firmware doesn't connect to WiFi
- Check that the SSID/Password fields match your firmware's WiFi configuration
- The fields must be set **before** clicking Load Firmware

### Network timeout after WiFi connects
- Verify the WebSocket proxy is running: `python3 tools/ws-net-proxy.py`
- Verify TAP is up: `ip addr show tap0`
- Click **Connect** under *Network proxy* in the browser before clicking Run
- Check proxy output for "Client connected" message

### Staircase text in terminal
- This is fixed by `convertEol: true` in the terminal config. If you see it, hard-refresh the page (Ctrl+Shift+R)

## File Structure

```
www/
├── index.html          # Main page: controls, terminal, live dashboard
├── app.js              # UI orchestration, worker communication
├── worker.js           # Web Worker: WASM emulation loop, WebSocket networking
└── pkg/                # wasm-pack output (generated)
    ├── esp_emu.js      # JS bindings
    └── esp_emu_bg.wasm # WASM binary

tools/
├── setup-tap.sh        # Create/teardown TAP interface with NAT
└── ws-net-proxy.py     # Python WebSocket-to-TAP proxy (requires websockets package)
```
