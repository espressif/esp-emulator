# `test_apps/` — ESP-IDF firmware the emulator is tested against

Each directory is a real ESP-IDF project that esp-emu boots through the full
chain — mask ROM → 2nd-stage bootloader → app — on every supported chip.

| App | What it exercises | Chips |
|---|---|---|
| `smoke/` | system info, NVS, flash, mmap, timers, FreeRTOS, pthread, esp_event, GPIO/RMT/LEDC/PCNT/I2C, crypto (PSA, SHA/AES/HMAC/ECDSA), SMP, WiFi + DHCP, Ethernet + DHCP, BLE advertising, reset reasons, task watchdog, PIE against recorded silicon results (P4, S31), ESP32-S3 `ee.*` SIMD instructions against recorded silicon results | all |
| `http_throughput/` | HTTP download throughput over WiFi (Ethernet on P4) | c3, c5, c6, p4, s3, s31 |
| `tee/` | ESP-TEE: secure storage, attestation, TEE/REE isolation | c5, c6, h2 |
| `flash_enc/` | flash encryption: key generation, first-boot encryption, encrypted reads | all |
| `flash32/` | 32 MiB flash (`--flash-size`) | c3, c5, p4, s31 |
| `psram/` | external RAM (`--psram-size`) | p4, s31 |
| `coremark/` | CoreMark benchmark: emulation speed per chip | all |
| `wpa3_sta/` | WPA3-SAE station against the built-in soft AP, repeated init cycles | c3, c5, c6, s3 |
| `unmapped_probe/` | loads and stores outside ordinary memory: which ones fault, which read 0, as measured on an ESP32-S3 board (`expected_esp32s3.txt`) | s3 |

## Running the smoke app

`run_smoke.sh` builds `smoke/` per chip and runs it in the `esp-emu` on your
`PATH`. It passes when at least one chip actually ran and every run's final
`RESULT_SUMMARY:` line reports no failures. Builds use the
`espressif/idf:latest` docker image by default; pass `--idf-path` to use a
sourced ESP-IDF checkout instead.

An unknown `--chips` name is rejected as a usage error (exit 2). A chip the
chosen IDF cannot target is reported as `SKIP`; when every requested chip is
skipped, the script fails (exit 1), so a typo cannot look like a pass.

If the IDF backend cannot be reached, the script prints the cause and exits 2.
This covers docker missing, the docker daemon not running or not permitted,
and an `--idf-path` directory without `export.sh`.

```sh
./install.sh                                   # esp-emu 0.46 or newer
test_apps/run_smoke.sh                         # every chip this IDF can build
test_apps/run_smoke.sh --chips esp32c3,esp32c6 # a subset
test_apps/run_smoke.sh --idf-path "$IDF_PATH"  # local ESP-IDF
```

Build output and the emulator log for each chip land under `./out/smoke-<chip>/`
(`--out DIR` to change). A chip the chosen IDF cannot target is reported as
`SKIP`. `.github/workflows/release-smoke.yml` runs the same script against
every published release with ESP-IDF master.

Per-chip pass counts differ because some cases only exist on some chips (BLE,
WiFi, Ethernet, SMP); a count that differs *between* chips is expected, a
chip's own count dropping is not.

## Building an app by hand

```sh
cp -r test_apps/smoke /tmp/smoke && cd /tmp/smoke
idf.py --preview set-target esp32c6      # --preview is needed for esp32s31, harmless otherwise
idf.py build && idf.py merge-bin -o build/merged.bin
esp-emu --chip esp32c6 --firmware build/merged.bin --elf build/emu_test.elf \
  --no-panic-intercept --timeout 240s --exit-on "RESULT_SUMMARY:"
```

`--elf` enables BLE interception and symbolicated backtraces;
`--no-panic-intercept` lets the app's deliberate task-watchdog panic reboot
the chip the way the test expects.
