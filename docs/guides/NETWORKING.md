# WiFi, Ethernet and Networking Backends

How firmware gets on a network under the emulator: the built-in WiFi soft AP (WPA2, WPA3, Enterprise), the OpenETH and ESP32-P4 Ethernet MACs, and the two backends that carry the traffic to the host: user-mode NAT (no setup) and TAP / vmnet (real LAN visibility).

## WiFi Emulation

The emulator includes a built-in WiFi soft access point with WPA2-PSK and WPA3-Personal (SAE) support. Firmware that connects to WiFi will:

1. **Scan** — The AP sends beacons and probe responses with the configured SSID
2. **Authenticate** — Open System, or SAE commit/confirm for WPA3
3. **Associate** — AP assigns AID=1
4. **4-way handshake** — Full EAPOL handshake with the PSK or SAE key (when password is set), including the IGTK for PMF
5. **DHCP** — Built-in DHCP server assigns 192.168.4.2 (gateway 192.168.4.1)

This works automatically — ESP-IDF WiFi station firmware will connect and receive an IP address. Use `--wifi-ssid` and `--wifi-password` to match your firmware's WiFi configuration. Default: SSID `myssid`, password `mypassword`.

With a password set, the AP advertises WPA2/WPA3 transition mode: the PSK and SAE key-management suites, PMF capable, and SAE hash-to-element. The station picks what it supports, so there is nothing to configure per firmware — a WPA2 station uses PSK, a WPA3 station uses SAE (hash-to-element or hunting-and-pecking, as the firmware's `sae_pwe_h2e` asks), and firmware with `threshold.authmode = WIFI_AUTH_WPA3_PSK` and `pmf_cfg.required = true` connects as it would to a real WPA3 network. SAE PMKSA caching is supported, so a reconnect skips the SAE exchange. Enterprise networks are covered below.

**Enterprise (802.1X)**: `--wifi-auth wpa3-enterprise --wifi-radius 127.0.0.1:1812,testing123` makes the AP an 802.1X authenticator that relays EAP to a RADIUS server and takes the PMK from the MSK it returns, so EAP-TLS, PEAP and TTLS all work with whatever the server supports. hostapd's built-in EAP server is enough, using the certificates from ESP-IDF's `examples/wifi/wifi_enterprise`:

```
# hostapd.conf
driver=none
interface=lo
eap_server=1
radius_server_clients=clients          # "127.0.0.1 testing123"
radius_server_auth_port=1812
eap_user_file=eap_users                # "* PEAP,TTLS,TLS" and "\"espressif\" MSCHAPV2,TTLS-MSCHAPV2 \"test11\" [2]"
ca_cert=ca.pem
server_cert=server.crt
private_key=server.key
```

`hostapd hostapd.conf &`, then run the example with `--wifi-ssid ESP_ENTERPRISE_AP` and the two flags above. `wpa2-enterprise` advertises the 802.1X AKM without PMF; `wpa3-enterprise` uses 802.1X-SHA256 with PMF required. Suite-B 192-bit is not supported.

To test a firmware's security policy rather than its connection, pin the AP: `--wifi-auth wpa3-sae` is a WPA3-only network (PMF required, a PSK association is refused), `--wifi-auth wpa2-psk` is the pre-0.45 WPA2-only AP that WPA3-only firmware must reject, and `--wifi-sae-pwe hunt-and-peck` hides hash-to-element so a firmware set to `WPA3_SAE_PWE_HASH_TO_ELEMENT` fails as it would on a legacy AP.

For real network connectivity, run with either [User-mode Networking](#user-mode-networking-no-host-setup) (zero setup, the default when `--net` is omitted and no `tap0` is available) or [TAP Networking](#tap-networking) (bridged to host interface; picked automatically on Linux when `tap0` is set up).

## Ethernet Emulation (OpenETH)

The emulator includes an OpenCores Ethernet MAC, the same virtual NIC used by the Espressif QEMU fork. ESP-IDF firmware built with `CONFIG_ETH_USE_OPENETH=y` will use this driver for networking instead of WiFi.

OpenETH provides a simpler, faster networking path — raw Ethernet frames pass directly between the firmware and the TAP device without 802.11 frame wrapping or WPA2 encryption overhead.

```sh
# Build firmware with OpenETH (in your ESP-IDF project sdkconfig):
#   CONFIG_ETH_USE_OPENETH=y
#   CONFIG_EXAMPLE_CONNECT_ETHERNET=y

# Run with TAP networking (also requires dnsmasq for DHCP)
sudo dnsmasq --interface=tap0 --bind-interfaces --dhcp-range=192.168.4.2,192.168.4.100,12h
esp-emu \
  --chip esp32c6 \
  --firmware build/merged_flash.bin \
  --net "tap,ifname=tap0"
```

The routing is automatic: when firmware enables OpenETH (sets TXEN/RXEN in MODER), TAP frames go directly to the Ethernet MAC. When firmware uses WiFi instead, frames route through the WiFi soft AP as before. No CLI flag is needed to select the path.

## ESP32-P4 Support

ESP32-P4 is a dual-core RV32IMAFC chip with hardware single-precision floating point, a custom CLIC interrupt controller, and a Synopsys DesignWare GMAC at `0x50098000`. The emulator boots ROM → 2nd-stage bootloader → ESP-IDF app, runs SMP firmware (`xTaskCreatePinnedToCore` on both cores), and supports the built-in EMAC over any networking backend (`--net user`, TAP, vmnet).

```sh
esp-emu \
  --chip esp32p4 \
  --firmware build/merged_flash.bin \
  --net user
```

Build firmware with `idf.py set-target esp32p4 && idf.py build`. The emulator models ESP32-P4 v3.x silicon. For firmware built for an earlier revision, run it with an eFuse file stating that revision (see [Chip Revision (eFuse)](CLI.md#chip-revision-efuse)).

CPU1 is brought up dynamically once the firmware releases its reset (`LP_AON_CLKRST_HPCPU_RESET_CTRL0`); single-core firmware (`CONFIG_FREERTOS_UNICORE=y`) runs on hart 0 only with no dual-core overhead. PSRAM is backed as zero-init RAM; PTP, jumbo Ethernet frames, and the LP core are not modelled.

## User-mode Networking (no host setup)

`--net user` enables a QEMU-style user-mode backend that proxies the guest's
TCP/UDP flows through host sockets. No TAP device, no `sudo`, no `dnsmasq` —
it works out of the box on Linux, macOS, and other Unix platforms.

```sh
esp-emu \
  --chip esp32c6 \
  --firmware build/merged_flash.bin \
  --net user
```

Addressing matches the WiFi soft AP (`192.168.4.0/24`, guest `192.168.4.2`,
gateway `192.168.4.1`). Traffic to the gateway IP is redirected to
`127.0.0.1` on the host, mirroring slirp's `10.0.2.2` convention.

### What's included

- **DHCP** + **ARP** so the guest can get an IPv4 lease on both WiFi and
  OpenETH paths
- **TCP/UDP outbound NAT** via smoltcp's TCP stack + non-blocking host sockets
- **Built-in DNS forwarder**: parses `/etc/resolv.conf`; the guest is told the
  gateway is its DNS server and queries are forwarded transparently
- **mDNS relay**: binds `224.0.0.251:5353` on the host (via `SO_REUSEPORT`, so
  it coexists with Avahi / systemd-resolved) and proxies multicast queries
- **IPv6**: smoltcp IPv6 stack, link-local + ULA addresses, Router Advertisement
  so the guest auto-configures a global IPv6 via SLAAC
  (`fd00:6573:702d:656d::/64`)
- **ICMP echo-reply spoof** — `ping` from the guest "succeeds" without real
  ICMP leaving the host (same as QEMU/libslirp without `CAP_NET_RAW`)

### Host → guest via `hostfwd` (QEMU-compatible syntax)

```sh
esp-emu ... --net "user,hostfwd=tcp::18080-:80,hostfwd=udp::1053-:53"

# From the host:
curl http://127.0.0.1:18080/hello
dig @127.0.0.1 -p 1053 example.com
```

Format: `hostfwd=PROTO:[HOST_ADDR]:HOST_PORT-:GUEST_PORT`. An empty host addr
(`tcp::10080-:80`) binds `0.0.0.0` (any interface) — match QEMU. Use
`tcp:127.0.0.1:10080-:80` to restrict to localhost.

### Firmware running its own soft AP

When the firmware starts a soft AP instead of connecting as a station, the
emulator joins it as a station, leases an address from the firmware's DHCP
server, and `hostfwd` TCP rules reach the firmware through that link. Open and
WPA2-PSK APs are supported; the SSID comes from the firmware's beacon, and the
password from `--wifi-fw-ap-password` (default: `--wifi-password`):

```sh
esp-emu --chip esp32s31 --firmware captive_portal.bin --wifi-fw-ap-password esp32_pwd \
  --net "user,hostfwd=tcp::18080-:80"
curl http://127.0.0.1:18080/
```

Verified with IDF's `captive_portal` example on ESP32-C3, C5, C6, S3 and S31.
An APSTA firmware gets both paths at once: its station on the emulator's AP
as usual, and the emulator on its AP. A new `hostfwd` connection goes through
the firmware's AP while that path is up and through its station otherwise, so
softAP provisioning with `esp_prov --transport softap --service_name
127.0.0.1:18080` works end to end. UDP `hostfwd` rules only reach the firmware
through its station. WPA3-SAE-only and hidden-SSID APs are not supported, and a
wrong password stops the station after three failed handshakes.

### mDNS NAT (Matter, HomeKit, etc.)

Zero-setup Matter commissioning and control — no TAP needed. Adds on top of the
basic mDNS relay by **rewriting** the guest's A/AAAA records to host loopback
and automatically binding hostfwd listeners for each SRV-advertised port.

```sh
esp-emu ... --net "user,mdns-nat=yes"

# In another terminal:
chip-tool pairing ble-wifi 1 myssid mypassword 20202021 3840 --ble-controller 1
chip-tool onoff toggle 1 1
```

When the guest announces `_matter._tcp … port=5540 target=<name>.local` with
A/AAAA records pointing at `192.168.4.2` / `fd00:...`, the backend:

1. Rewrites A → `127.0.0.1`, AAAA → `::1` in the mDNS response (both the
   multicast announcements and the unicast replies to QU-bit queries).
2. Extracts the SRV port and auto-binds `127.0.0.1:5540` UDP + TCP hostfwds
   that forward to the guest.

chip-tool's Matter resolver sees the service at `127.0.0.1:5540`, the hostfwd
bridges the CASE/operational traffic into the guest, and post-commissioning
control (toggle, read, invoke, etc.) works without any kernel-level routing.
This is beyond what QEMU's libslirp does — QEMU silently drops all mDNS.

### Restrict mode

```sh
esp-emu ... --net "user,restrict=yes"
```

Blocks all guest-initiated TCP/UDP except DNS. Useful for CI to prove a
firmware doesn't phone home.

### Limitations vs. TAP

- **Not reachable from the host by IP**: `ping 192.168.4.2` does not work.
  The guest IP lives inside the emulator process; use `hostfwd` to expose
  specific ports instead.
- **No packet capture on a host interface**: `tcpdump` sees nothing because
  the frames never leave user space. Use `RUST_LOG=esp_emu::net_user=trace`
  for backend-level tracing.
- Not yet implemented: IP fragmentation, TFTP server, `guestfwd`.

## TAP Networking

TAP mode bridges the emulated network (WiFi or Ethernet) to a host network interface, giving the firmware real TCP/IP connectivity. WiFi firmware uses the built-in DHCP server; Ethernet (OpenETH) firmware needs an external DHCP server on the TAP interface (e.g., dnsmasq). Use this mode when you need real LAN visibility (`ping`, `tcpdump`, multi-emulator bridging) — otherwise `--net user` above is easier.

### Linux (TAP)

```sh
# Create TAP interface with NAT (auto-detects outbound interface)
sudo ./tools/setup-tap.sh

# Run with TAP
esp-emu \
  --chip esp32c3 \
  --firmware app.bin \
  --net "tap,ifname=tap0"

# Cleanup
sudo ./tools/setup-tap.sh --teardown
```

### macOS (vmnet)

```sh
# vmnet requires sudo or com.apple.vm.networking entitlement
sudo esp-emu \
  --chip esp32c3 \
  --firmware app.bin \
  --net vmnet
```
