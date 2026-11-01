# BLE Emulation

Running NimBLE and Bluedroid firmware against a virtual controller (Bumble) or a physical Linux HCI adapter. Matter over BLE is covered in [MATTER.md](MATTER.md).

BLE firmware runs natively in the emulator: the NimBLE host stack on every BLE chip, and Bluedroid on ESP32-S31 (its BTDM controller's standard VHCI driver is intercepted the same way). HCI commands are either handled by a built-in virtual controller or forwarded to an external backend via `--ble-hci`. Requires `--elf` to provide firmware symbols for HCI interception.

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

The test script scans, connects, discovers GATT services, reads/writes characteristics, and subscribes to notifications. Works with ESP32-C3, ESP32-C6, ESP32-H2, and ESP32-S3.

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
