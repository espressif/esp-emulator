# Browser Emulation Guide

Run ESP32-C3/C5/C6/H2/P4/S3/S31 firmware in a web browser using the WASM build of ESP-EMU. Everything runs inside the tab: the firmware image never leaves your machine.

## Hosted page

Every release is deployed to GitHub Pages at **<https://espressif.github.io/esp-emulator/>** by the public mirror's `publish-pages` workflow. It is the same `www/` bundle as the release's wasm tarball, served as static files; nothing runs server-side. For networking, run `esp-emu-relay` on your computer (see [Browser Networking](#browser-networking)); the hosted page connects to it at `ws://127.0.0.1:8787`.

## Quick Start

Download `esp-emu-<version>-wasm.tar.gz` from the [latest release](https://github.com/espressif/esp-emulator/releases/latest), unpack it, and serve its `www/` folder:

```sh
tar xzf esp-emu-<version>-wasm.tar.gz
cd esp-emu-<version>-wasm/www
python3 serve.py 8080
```

Open `http://localhost:8080` in Chrome/Firefox/Safari. `serve.py` sends
no-cache headers, so a reload always loads what is in the folder.

### Load and run firmware

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

## Virtual parts

The browser build can add virtual components to the chip: a push button on a
GPIO, an SHT31 sensor on I2C, a TMP123 sensor on SPI. `button-demo.html` and
`sensor-demo.html` are the examples. See [PARTS.md](PARTS.md) for how to run
them, how to write a part of your own, and how to use parts in your own page.

## Browser Networking

By default, the browser emulator runs in internal-only mode: WiFi association and DHCP work, but the firmware has no path to the real network. The **Network** panel connects it to `esp-emu-relay`, a small program that runs on Linux, macOS or Windows. A custom page can also bridge raw frames to a Linux TAP interface (see [TAP proxy](#tap-proxy)).

### Relay

The emulator's NAT ends the firmware's TCP and UDP inside the tab. The WASM module opens one WebSocket to the relay for each TCP flow and for each UDP flow (one firmware port to one server address and port). The relay opens the connection to the host. The firmware's DNS queries go to the relay's `/dns`, which answers them with the operating system's resolver. The page clock answers SNTP. TLS stays end to end between the firmware and the server. The relay can also give connections on its own ports to a server in the firmware (see [Port forwards](#port-forwards)).

```
Browser (WASM emulator, NAT)
  ↕ one WebSocket per TCP or UDP flow, GET /dns per DNS query, /listen for port forwards
esp-emu-relay (on your computer)
  ↕ TCP, UDP, system resolver
Internet
```

#### 1. Get the relay

Click the download button in the **Network** panel. The page selects the archive for your OS and CPU, from the relay release the page was built for. **Other OS downloads** lists the other archives. Safari and Firefox do not tell a page the CPU of a Mac, so the page then selects Apple silicon. On an Intel Mac, select **macOS (Intel)** from the list.

Or install it from a terminal. The **Network** panel shows the command for your OS, for that relay release. With no `--version`, the commands below install the relay release the installer was published with. On Linux and macOS, it goes to `~/.local/bin` (`--bin-dir` changes it):

```sh
curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh | sh -s -- --relay
```

On Windows, from PowerShell, it goes to `%LOCALAPPDATA%\Programs\esp-emu-relay`, which the script adds to your user `PATH`:

```powershell
irm https://raw.githubusercontent.com/espressif/esp-emulator/main/install-relay.ps1 | iex
```

The relay has its own version and its own GitHub release, `relay-vX.Y.Z`, apart from the emulator's: a relay works with every page of the same `MAJOR`, and the page says so when it finds another. Each relay release has `esp-emu-relay-<version>-<target>.tar.gz` for `x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu`, `x86_64-apple-darwin` and `aarch64-apple-darwin`, and `esp-emu-relay-<version>-x86_64-pc-windows-gnu.zip`. The macOS binaries are signed and notarized. To build the relay from a checkout:

```sh
cargo build --release --manifest-path esp-emu-relay/Cargo.toml
./esp-emu-relay/target/release/esp-emu-relay
```

#### 2. Start it

```sh
./esp-emu-relay
```

It listens on `127.0.0.1:8787` and prints the URL to use: `ws://127.0.0.1:8787`.

#### 3. Connect from the browser

1. In the **Network** panel, keep the URL `ws://127.0.0.1:8787`. The page remembers a URL that you change.
2. Click **Connect**.
3. Click **Run**. The panel shows the open connections, and the terminal shows each host that the relay refuses.

A page served over HTTPS (such as the hosted page) can use `ws://127.0.0.1` and `ws://localhost`, because browsers treat loopback addresses as secure. For a relay on another machine, an HTTPS page needs `wss://`: start the relay with `--tls-cert` and `--tls-key`, or put it behind a TLS reverse proxy.

#### Relay options

| Flag | Default | Description |
|------|---------|-------------|
| `--port N` | `8787` | Listen port |
| `--bind ADDR` | `127.0.0.1` | Listen address |
| `--allow-origin PATTERN` | any | Accept only pages from PATTERN, such as `https://*.example.com` |
| `--allow-host PATTERN` | any public host | Connect only to host names that match PATTERN, such as `*.example.com` |
| `--allow-port N` | any | Connect only to port N |
| `--allow-private` | off | Also connect to loopback, private and link-local addresses. Needs `--allow-origin` and a loopback `--bind` |
| `--forward HPORT:FPORT` | — | Give connections to port HPORT on the `--bind` address to port FPORT of the firmware. Needs `--allow-origin` |
| `--max-flows N` | `16` | Open flows per client address |
| `--idle-timeout SECS` | `300` | Close a flow with no data for SECS |
| `--tls-cert FILE` | — | Serve `wss://` and `https://` with this PEM certificate chain |
| `--tls-key FILE` | — | The PEM private key of `--tls-cert` |

Each `--allow-*` option and `--forward` repeats or takes a comma-separated list. `*` matches one DNS label.

**Caution:** with no `--allow-origin`, any web page open in your browser can use the relay. Without `--allow-private`, the relay never connects to a loopback, private, link-local, carrier-NAT, multicast or reserved address, also after a DNS lookup, so a page cannot reach your LAN through it. Use `--allow-origin` on a shared machine or with `--bind 0.0.0.0`.

#### This computer and your network

To let the firmware reach a server on this computer (such as a local MQTT broker) or on your LAN, start the relay with `--allow-private` and the origin of the emulator page:

```sh
./esp-emu-relay --allow-private --allow-origin https://espressif.github.io
```

The firmware reaches this computer's loopback address through its gateway address, `192.168.4.1`: `mqtt://192.168.4.1:1883` goes to port 1883 of `127.0.0.1` on the computer that runs the relay. This is the same as with `--net user` in the native emulator. LAN addresses and names that resolve to them work as they are.

**Caution:** `--allow-private` lets the pages that `--allow-origin` accepts reach every server on this computer and on your LAN. The relay refuses `--allow-private` without `--allow-origin`, and with a `--bind` address that is not loopback.

#### Port forwards

To connect to a server in the firmware (such as the HTTP server example), start the relay with `--forward` and the origin of the emulator page:

```sh
./esp-emu-relay --forward 8080:80 --allow-origin https://espressif.github.io
```

Then open `http://127.0.0.1:8080/` in the browser. The **Network** panel shows each forward while the relay is connected. The relay gives each connection to the newest page that is connected to it. The relay refuses `--forward` without `--allow-origin`, because any other page could take the connections. With no page, or with no firmware on the network, the relay closes the connection. A firmware port with no server resets the connection.

#### Relay API

| Method | Description |
|--------|-------------|
| `net_set_relay(url)` | Relay the firmware's TCP, UDP, DNS and SNTP through the relay at `url` (`ws://`, `wss://`, `http://` or `https://`), and take the relay's port forwards. Turns the NAT on and resets the open flows; the page gets a `TCP_CLOSE` abort for each flow that it held. `""` stops the relay. While a relay is set, `net_event_drain` returns only the `UDP6` events and those aborts |
| `net_relay_state()` | `{ connected, unreachable, flows, refused: [{ host, port, udp, status, reason }], forwards: [{ host_port, guest_port }] }`, or `null` with no relay set |

#### Relay protocol

A page that does not use `net_set_relay` can speak to the relay itself:

| Request | Answer |
|---------|--------|
| WebSocket `GET /tcp?host=H&port=P` | Binary messages carry the bytes of one TCP flow to `H:P`. An empty message is a FIN from its sender, and the other side can still send. The relay closes with code 1000 after a FIN from each side. A close from the page with code 1000 ends the flow with a FIN to the host; a close with another code or no code resets the flow |
| `…&credit=1` on `/tcp` or `/accept` | Flow control: the relay sends at most 256 KiB of host bytes that the page has not credited. The page credits bytes with a text message that holds their count in decimal, such as `65536`. Without `credit=1`, a text message closes the flow with code 1003 |
| WebSocket `GET /udp?host=H&port=P` | Each binary message is one UDP datagram to or from `H:P`. The relay closes with code 1000 after the idle timeout |
| `GET /tcp?host=H&port=P` or `GET /udp?host=H&port=P` (no upgrade) | `204` when the relay accepts the target, or the status and reason (`403 address not public`, `403 origin not allowed`, `429 too many flows`, …). A browser hides why a WebSocket failed, so a page asks here |
| WebSocket `GET /listen` | The relay sends `[1][HPORT u16][FPORT u16]` (big-endian) for each forward, then `[2][id u32][FPORT u16]` for each connection to a forward port. A newer `/listen` takes the forwards, and the relay closes the older one with code 4000 |
| WebSocket `GET /accept?id=N` | Carries connection N as `/tcp` carries a flow. The relay closes a connection that no page claims in 10 s |
| `GET /version` | `{"name":"esp-emu-relay","version":"X.Y.Z"}` for any origin. The page asks after it connects and warns when the `MAJOR` is not the one it was built for |
| `GET /dns?dns=Q` | `Q` is a DNS query in base64url, as RFC 8484 GET uses. The answer is an `application/dns-message` with the public IPv4 addresses (A). AAAA gets an empty answer; other types get NOTIMP |

### TAP proxy

The bundled page has no TAP mode. A custom page can send the frames of `wifi_tx_drain()` to `ws-net-proxy` over a WebSocket and give each frame that it receives to `wifi_rx_push(frame)`, with the NAT off. The firmware then has a real Linux network interface: ICMP, inbound connections and mDNS work. The proxy and the TAP script are in this repository's `tools/`, not in a release, so run these steps from a checkout.

#### 1. Create TAP interface with NAT

```sh
sudo ./tools/setup-tap.sh
```

This creates `tap0` with IP `192.168.4.1/24`, enables IP forwarding, and adds iptables NAT rules. Run `sudo ./tools/setup-tap.sh --teardown` to clean up.

#### 2. Start the WebSocket proxy

```sh
cargo build --release --features ws-proxy --bin ws-net-proxy
./target/release/ws-net-proxy
```

It listens on `ws://localhost:8765` by default.

#### 3. Connect from the page

Open a binary WebSocket to `ws://localhost:8765`. Each message is one Ethernet frame. `wifi_tx_drain()` returns the firmware's frames, each with a `u32` little-endian length before it.

#### Proxy options

| Flag | Default | Description |
|------|---------|-------------|
| `--tap NAME` | `tap0` | TAP interface name |
| `--port PORT` | `8765` | WebSocket listen port |
| `--host ADDR` | `0.0.0.0` | Bind address |

#### Verifying connectivity

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

## BLE Central API

The built-in BLE controller has a virtual phone (a BLE central) that JavaScript drives. The emulator ends the link layer and L2CAP. JavaScript sends and receives ATT PDUs, so a `navigator.bluetooth` replacement does GATT itself. Load the app ELF with `load_app_elf` first: BLE interception needs its symbols.

| Method | Description |
|--------|-------------|
| `ble_scan(enable)` | Turn the scanner on or off |
| `ble_connect(addr)` | Connect to an advertiser. `addr` is 6 bytes, MSB first. The result comes as a `CONNECTED` event |
| `ble_disconnect(conn)` | End the link. The firmware gets Disconnection Complete, reason 0x13 |
| `ble_att_send(conn, pdu)` | Send one ATT PDU on CID 0x0004. The emulator fragments it into ACL packets |
| `ble_event_drain()` | All events since the last call: `[u32 len LE][u8 type][body]...`. `len` covers the type and the body |

| Type | Event | Body |
|------|-------|------|
| `0x01` | `ADV_REPORT` | `addr[6]`, `addr_type u8`, `connectable u8`, `rssi i8`, `adv_len u8`, `adv[]`, `rsp_len u8`, `rsp[]` |
| `0x02` | `CONNECTED` | `conn u16`, `addr[6]`, `status u8` (0 = OK; 0x3E and `conn` 0xFFFF when no connectable advertiser has `addr`) |
| `0x03` | `DISCONNECTED` | `conn u16`, `reason u8` (0x16 after `ble_disconnect`; the firmware's reason when it disconnects; 0x08 after a chip reset) |
| `0x04` | `ATT_RX` | `conn u16`, `pdu[]` (one reassembled SDU from CID 0x0004) |

An `ADV_REPORT` comes when scanning starts, when the firmware turns advertising on, and when it changes its advertising or scan response data. There is no event when advertising stops. The central accepts each L2CAP connection parameter update and replies Pairing Not Supported to SMP. It has no supervision timeout, so JavaScript can pause between batches. One connection at a time is supported.

### HCI forwarding

A BLE host stack in the page (for example Bumble in Pyodide) can take the firmware's HCI instead of the virtual central, as `--ble-hci tcp:` does natively. The firmware's packets are not processed in the emulator: the page carries them both ways.

| Method | Description |
|--------|-------------|
| `enable_ble_forwarding()` | Forward the firmware's HCI to the page. Call it before `load_app_elf` |
| `ble_hci_tx_drain()` | HCI packets from the firmware since the last call: `[u32 len LE][H4 packet]...` |
| `ble_hci_has_tx()` | Whether `ble_hci_tx_drain` has packets |
| `ble_hci_rx_push(packet)` | One H4 packet from the page's controller to the firmware. ACL longer than 251 bytes is fragmented |

## Browser NAT API

The emulator can end the firmware's IPv4 TCP and UDP itself, so the page needs no TAP and no frame proxy. Each TCP flow and each UDP datagram comes to JavaScript as an event. The page carries TCP to a byte relay (for example a WebSocket to TCP relay) and answers UDP itself (DNS, SNTP). `net_set_relay` (see [Relay API](#relay-api)) does all of this inside the module. TLS stays end to end. The soft AP still answers DHCP and ARP, and the emulator answers ICMP echo. On an Ethernet NIC (OpenETH, DW GMAC) the NAT answers DHCP and ARP itself. IPv6 is not routed.

The page can also be a peer on the firmware's link, for example a Matter commissioner after BLE provisioning. It opens UDP sockets on the NAT's link-local IPv6 address and can join IPv6 multicast groups, such as `ff02::fb` for mDNS. The NAT answers Neighbor Discovery for its own address only.

| Method | Description |
|--------|-------------|
| `net_enable(on)` | Send the firmware's frames to the NAT instead of `wifi_tx_drain`, which returns nothing while the NAT is on. `false` closes every open flow with a `TCP_CLOSE` abort |
| `net_event_drain()` | All events since the last call, in the BLE event format |
| `net_tcp_connect(guest_port)` | Open a TCP connection from the NAT to the firmware's `guest_port`, for a connection from the network, and return the flow ID. It has no `TCP_OPEN`; otherwise it is a flow as the firmware's are. A firmware port with no server gives a `TCP_CLOSE` abort |
| `net_tcp_opened(id, ok)` | The relay connected the flow, or failed. A failure resets the firmware's connection |
| `net_tcp_send(id, data)` | Bytes from the remote host to the firmware |
| `net_tcp_close(id, abort)` | The remote host sent FIN (`false`) or failed (`true`). After a FIN the firmware can still send |
| `net_udp_send(guest_port, src_ip, src_port, data)` | One datagram to the firmware's `guest_port`, from the 4-byte `src_ip` and `src_port` |
| `net_local_ipv6()` | The NAT's 16-byte link-local IPv6 address, which the `net_udp6_*` sockets send from |
| `net_udp6_open(port)` | Open a UDP socket on the NAT's IPv6 address and return its ID. `0` picks a free port. A port opens once |
| `net_udp6_send(id, dst_ip, dst_port, data)` | One datagram from socket `id` to the 16-byte `dst_ip` and `dst_port` |
| `net_udp6_close(id)` | Close socket `id` |
| `net_join_multicast(group)` | Receive the 16-byte IPv6 multicast `group` on the sockets |

| Type | Event | Body (little-endian) |
|------|-------|------|
| `1` | `TCP_OPEN` | `id u32`, `dst_ip[4]`, `dst_port u16` |
| `2` | `TCP_DATA` | `id u32`, bytes from the firmware |
| `3` | `TCP_CLOSE` | `id u32`, `abort u8` (0 = firmware FIN after all its data, 1 = firmware reset or flow removed). An abort can follow a FIN for the same flow |
| `4` | `UDP` | `guest_port u16`, `dst_ip[4]`, `dst_port u16`, payload |
| `5` | `UDP6` | `id u32`, `src_ip[16]`, `src_port u16`, payload: a datagram to socket `id` |

The NAT completes the firmware's handshake at once and holds the firmware's bytes until `net_tcp_opened(id, true)`. Flow IDs are not used again, also after `net_enable(false)` or `restart()`. Calls for a flow that is gone do nothing, because a close can cross a call from the page. `restart()` closes every open flow with an abort. The `net_udp6_*` sockets stay open across `restart()` and `net_enable(false)`. A flow whose firmware stays silent for 150 s, keep-alive probes included, closes with an abort. The NAT keeps the host's bytes for the firmware with no limit, so a fast download into slow firmware uses page memory. With `net_set_relay`, the relay's flow control holds each flow to 256 KiB. The methods throw when the network is off and on bad arguments.

## Limitations

- **A host program** — Real networking needs `esp-emu-relay` on the host
- **Relay: IPv4 only, TCP port forwards only** — a UDP server in the firmware gets datagrams only from a host that it sent to first. A datagram larger than 1472 bytes does not reach the firmware. A UDP flow with no datagram for 60 s closes, and after a refusal the datagrams to that target are dropped for 30 s
- **Performance** — WASM runs ~2-5x slower than native. TCP-heavy workloads may experience higher latency
- **Pacing** — Emulated time is held to the host clock: when firmware idles (WFI, `vTaskDelay`) the worker waits the difference in `setTimeout` instead of running ahead, so delays and timeouts take real time as on a board. With the network attached each wait is capped at 5 ms so incoming frames are not left queued
- **TAP proxy: custom pages, Linux only** — The bundled page has no TAP mode. The proxy supports one browser tab at a time per TAP interface

## Troubleshooting

### "WASM module loaded" never appears
- Make sure you're serving via HTTP (`python3 serve.py 8080` in `www/`), not opening `index.html` directly (`file://` URLs block WASM imports)

### Firmware doesn't connect to WiFi
- Check that the SSID/Password fields match your firmware's WiFi configuration
- The fields must be set **before** clicking Load Firmware

### "Cannot reach the relay"
- Start `esp-emu-relay` and keep its window open
- Check that the URL in the **Network** panel is the URL that the relay printed
- From an HTTPS page, use `ws://127.0.0.1`, `ws://localhost` or a `wss://` URL

### "The relay refused HOST:PORT: REASON"
- `address not public`: the host resolves to a loopback, private or reserved address, or it is the gateway address. Start the relay with `--allow-private` and `--allow-origin` (see [This computer and your network](#this-computer-and-your-network))
- `host not allowed` or `port not allowed`: the relay runs with `--allow-host` or `--allow-port`, and the target is not on the list
- `origin not allowed`: the relay runs with `--allow-origin`, and this page is not on the list

### Staircase text in terminal
- This is fixed by `convertEol: true` in the terminal config. If you see it, hard-refresh the page (Ctrl+Shift+R)

## File Structure

```
www/
├── index.html          # Main page: controls, terminal, live dashboard
├── app.js              # UI orchestration, worker communication
├── relay-download.js   # Picks the esp-emu-relay archive for the visitor's OS and CPU
├── worker.js           # Web Worker: WASM emulation loop, relay URL
├── button-demo.html    # Virtual parts demo: C3 firmware + a push button (PARTS.md)
├── sensor-demo.html    # Virtual parts demo: C3 firmware + an I2C sensor (PARTS.md)
├── cosim.js            # CosimPump: connects the emulator to each part's worker
├── part-worker.js      # Web Worker: runs one part, a WebAssembly module
├── vendor/
│   └── esp_cosim_pb.js # JavaScript bindings for proto/esp_cosim.proto
└── pkg/                # the emulator, as WebAssembly
    ├── esp_emu.js      # JS bindings
    └── esp_emu_bg.wasm # WASM binary

tools/
├── relay/              # esp-emu-relay: WebSocket relay for TCP and UDP, its own Cargo workspace
├── setup-tap.sh        # Create/teardown TAP interface with NAT
└── ws-net-proxy.rs     # WebSocket-to-TAP proxy for custom pages (--features ws-proxy --bin ws-net-proxy)
```
