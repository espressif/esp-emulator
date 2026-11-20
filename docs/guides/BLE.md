# BLE Emulation

Running NimBLE and Bluedroid firmware against a virtual controller (Bumble) or a physical Linux HCI adapter. Matter over BLE is covered in [MATTER.md](MATTER.md).

BLE firmware runs natively in the emulator: the NimBLE host stack on every BLE chip, Bluedroid on ESP32-S31 (its BTDM controller's standard VHCI driver is intercepted the same way), and Rust [esp-hal](https://github.com/esp-rs/esp-hal) / `esp-radio` firmware on ESP32-C3, C5, C6 and H2 (see [esp-hal / esp-radio firmware](#esp-hal--esp-radio-firmware)). HCI commands are either handled by a built-in virtual controller or forwarded to an external backend via `--ble-hci`. Requires `--elf` to provide firmware symbols for HCI interception.

## Bumble (software-only, no hardware needed)

[Google Bumble](https://github.com/google/bumble) acts as a virtual BLE controller and GATT client over TCP.

```sh
pip3 install bumble
python3 tools/bumble_test.py &        # Virtual controller on port 9544
esp-emu \
  --chip esp32c3 \
  --firmware build/merged_flash.bin \
  --elf build/bleprph.elf \
  --ble-hci tcp:localhost:9544
```

The test script scans, connects, discovers GATT services, reads/writes characteristics, and subscribes to notifications. Works with ESP32-C3, ESP32-C5, ESP32-C6, ESP32-H2, and ESP32-S3.

## Physical adapter (Linux)

Forward HCI to a real Bluetooth adapter for phone interaction:

```sh
sudo hciconfig hci0 down
sudo setcap 'cap_net_admin+eip' "$(command -v esp-emu)"
esp-emu \
  --chip esp32c3 \
  --firmware build/merged_flash.bin \
  --elf build/bleprph.elf \
  --ble-hci hci0
```

## Helper tools

Companion scripts in [`tools/`](../../tools/):

- `bumble_test.py` — virtual BLE controller (Google Bumble) over TCP for HCI testing
- `bumble_classic_peer.py` — virtual controller plus a Classic BT peer that drives ESP32-S31 Bluedroid examples (A2DP sink, SPP, L2CAP, HID, HFP) over BR/EDR
- `vhci_bridge.py` — Linux VHCI HCI bridge

## esp-hal / esp-radio firmware

Rust firmware built on [esp-hal](https://github.com/esp-rs/esp-hal) with `esp-radio`'s `ble` feature (a trouBLE host, for instance) links the same Espressif controller library as ESP-IDF but calls it directly, without IDF's transport layer. The emulator recognises that from the ELF — the controller's `r_ble_hci_trans_cfg_hs` on ESP32-C5/C6/H2, the BTDM library's `API_vhci_host_*` on ESP32-C3 — and intercepts one layer down, so nothing changes on the command line beyond `--elf`. Supported on ESP32-C3, C5, C6 and H2. The ESP32-S3 port uses the C3 path but has not been run; ESP32-S31 is not covered.

esp-radio's examples print over USB-Serial/JTAG, so pass `--console usb-jtag`:

```sh
# in an esp-hal checkout
cd examples/ble/bas_peripheral
cargo +nightly build --release --target riscv32imac-unknown-none-elf --features esp32c6
ELF=target/riscv32imac-unknown-none-elf/release/bas-peripheral
espflash save-image --chip esp32c6 --merge $ELF c6.bin
esp-emu --chip esp32c6 --firmware c6.bin --elf $ELF --console usb-jtag
```

For ESP32-C3 use `--target riscv32imc-unknown-none-elf --features esp32c3` and `--chip esp32c3`. Add `--ble-hci` as for IDF firmware. The log line `BLE: N functions intercepted (EspRadioNpl)` (C5/C6/H2) or `… (EspRadioBtdm)` (C3) confirms the ELF was recognised; a release build keeps the symbols, a stripped ELF cannot be intercepted. This example, run against `tools/bumble_test.py` (`--ble-hci tcp:localhost:9544`), advertises, accepts the connection, serves GATT discovery, reads, writes and notifications on ESP32-C3, C5, C6 and H2. For the other chips: `--features esp32h2` or `esp32c5` with the `riscv32imac` target.
