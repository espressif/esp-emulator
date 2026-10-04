# HTTP throughput benchmark

A self-contained HTTP download benchmark for esp-emu. The firmware brings up a
NIC, downloads a large payload (100 MB by default) from a local HTTP server, and
reports throughput measured in **emulated time** — so the number is stable
regardless of host load.

- **WiFi** path on chips with WiFi: **ESP32-C3, ESP32-C5, ESP32-C6** (STA →
  built-in WPA2 soft-AP). C5 is dual-band silicon, but the firmware is the same
  on every chip: the emulated AP is 2.4 GHz only, and the modelled radio answers
  the driver's stock `WIFI_BAND_MODE_AUTO` scan there.
- **Ethernet** path on chips with an internal EMAC: **ESP32-P4, ESP32-S31**
  (DesignWare GMAC + generic 802.3 PHY).

The link is selected at compile time from `soc_caps.h`, so one firmware source
covers every target.

## How it works

```
guest firmware  --HTTP GET-->  192.168.4.1:8070  ==user-net redirect==>  host 127.0.0.1:8070
   (esp_http_client)              (gateway IP)                              (http_server.py)
```

esp-emu's user-mode networking hands the guest `192.168.4.2` and redirects the
gateway IP (`192.168.4.1`) to the host loopback (slirp-style). So the firmware
just GETs `http://192.168.4.1:8070/bigfile` and reaches `http_server.py` on the
host — no TAP, no sudo, no external server.

Throughput is computed guest-side as `bytes * 8 / esp_timer_us` (1 bit/µs ==
1 Mbit/s). Because `esp_timer` measures emulated microseconds, the result does
not drift with host CPU load — unlike wall-clock timing.

The firmware prints one machine-readable line on success:

```
HTTP_TPUT: bytes=104857600 us=10479201 mbit_s=80.05 target=esp32p4 link=eth
HTTP_TPUT_RESULT: PASSED
```

## Prerequisites

- ESP-IDF environment (`. $IDF_PATH/export.sh`)
- A built `esp-emu` (`cargo build --release`)
- Python 3 (for the host server)

## Running

### 1. Start the host server

```sh
python3 http_server.py            # 100 MB on 127.0.0.1:8070/bigfile
python3 http_server.py 8070 10485760   # optional: PORT SIZE_BYTES (e.g. 10 MB)
```

### 2. Build for a target

```sh
idf.py -B build_esp32c3 set-target esp32c3   # esp32c5 / esp32c6 / esp32p4
idf.py -B build_esp32c3 build
idf.py -B build_esp32c3 merge-bin -o $PWD/build_esp32c3/merged.bin
```

For the preview target **esp32s31**, add `--preview` to `set-target`:

```sh
idf.py -B build_esp32s31 --preview set-target esp32s31
idf.py -B build_esp32s31 build
idf.py -B build_esp32s31 merge-bin -o $PWD/build_esp32s31/merged.bin
```

### 3. Run in the emulator

```sh
./../target/release/esp-emu \
  --chip esp32c3 \
  --firmware build_esp32c3/merged.bin \
  --elf build_esp32c3/http_throughput.elf \
  --net user \
  --timeout 300s \
  --exit-on "HTTP_TPUT_RESULT"
```

Use the matching `--chip` / build dir for each target. `--net user` is the
default fallback on Linux too, but pass it explicitly for determinism.

> **Wall-clock note:** the emulator runs ~10× slower than real time, so a
> 100 MB download takes a few minutes of wall-clock. That is expected and is
> *not* the reported throughput — the `mbit_s` figure is emulated-time and is
> the number to compare across chips. Drop the payload to 10 MB
> (`http_server.py 8070 10485760`) for quicker iteration.

## Reference numbers

Measured with a 100 MB payload, `--net user`, release build of esp-emu. Every
run transferred the full payload (`complete=1`). Absolute values scale with the
host and esp-emu version; the WiFi-vs-Ethernet ratio is the stable signal.

| Chip      | Link     | Throughput (emulated) |
|-----------|----------|-----------------------|
| ESP32-C3  | WiFi     | ~40 Mbit/s            |
| ESP32-C5  | WiFi     | ~55 Mbit/s ¹          |
| ESP32-C6  | WiFi     | ~37 Mbit/s            |
| ESP32-P4  | Ethernet | ~80 Mbit/s            |
| ESP32-S3  | WiFi     | ~45 Mbit/s ²          |
| ESP32-S31 | WiFi     | ~56 Mbit/s ³          |

² S3 measured at 20 MiB (44.54 Mbit/s); its lead over C3 tracks the
240/160 MHz clock ratio, like C5's.

³ S31 measured over WiFi at 8 MiB (56.46 Mbit/s) once its WiFi landed, for
parity with the other WiFi chips; over its DW GMAC Ethernet it measured ~78
Mbit/s, the same MAC the P4 row covers.

¹ C5 is measured at the harness default of **8 MiB**, not 100 MB. The figure is
still comparable: the same run gives C3 39.48 and C6 37.04 Mbit/s at 8 MiB,
i.e. within noise of their 100 MB entries, so payload size is not what separates
them. C5's lead tracks its clock — 54.82 / 37.04 = 1.48 against a 240/160 MHz
ratio of 1.50.

## Smoke harness (opt-in)

`tests/http_throughput_smoke.rs` automates the whole flow — builds the firmware
per chip, serves the payload from an in-process HTTP server (no `http_server.py`
needed; it's the Rust equivalent), boots each binary, and asserts the full
payload was downloaded. It is **opt-in**: plain `cargo test` skips it. Run it
explicitly with:

```sh
ESP_EMU_RUN_HTTP_TPUT=1 cargo test --release --test http_throughput_smoke -- --nocapture
```

Knobs: `ESP_EMU_IDF_CHIPS=esp32c3,esp32c5,esp32p4` (chip set),
`ESP_EMU_HTTP_TPUT_MB=100` (payload size, default 8). The harness asserts
completeness only and prints the emulated-time `mbit_s` for the record — it does
not gate on a throughput threshold (wall-clock would flake under host load).

## Files

- `main/http_throughput_main.c` — firmware: link bring-up + HTTP download + measurement.
- `http_server.py` — threaded host HTTP server streaming a fixed-size payload (for CLI runs).
- `sdkconfig.defaults` — large-app partition + enlarged lwIP TCP windows.
- `../tests/http_throughput_smoke.rs` — opt-in Rust smoke harness (see above).
