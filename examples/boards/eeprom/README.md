# EEPROM board: ESP-IDF's `hd_eeprom` example against a virtual 93C46

ESP-IDF's `examples/peripherals/spi_master/hd_eeprom` writes "Hello World!"
to an AT93C46D EEPROM and reads it back, run by `esp-emu` with no hardware.
The EEPROM is [`examples/parts/eeprom93c46`](../../parts/eeprom93c46)
compiled to WebAssembly; `board.json` wires it as the example wires it on an
ESP32-C3: chip select `CS` on GPIO 10, clock `SK` on 11, data in `DI` on 12,
data out `DO` on 13. The example is unchanged: its driver works the chip
select as a plain GPIO and waits for the chip's busy signal, and both reach
the part.

This board is not in the release's examples kit, since its firmware is
ESP-IDF's own example rather than one of this repository's test apps.

## Build the pieces

- The part: [`examples/parts/eeprom93c46`](../../parts/eeprom93c46) says
  how; the build gives `eeprom93c46.wasm`. Put it next to `board.json`.
- The firmware, with ESP-IDF 5.x: in a copy of
  `$IDF_PATH/examples/peripherals/spi_master/hd_eeprom`,
  `idf.py set-target esp32c3 build merge-bin -o merged_flash.bin` gives
  `build/merged_flash.bin` and `build/spi_eeprom.elf`.

## Run it

In this directory:

```sh
esp-emu --chip esp32c3 --firmware /path/to/build/merged_flash.bin \
    --elf /path/to/build/spi_eeprom.elf --cosim-board board.json \
    --exit-on "Example finished"
```

The log names the part as it attaches, then the example writes the string a
byte at a time, waiting for the chip's ready signal after each, and reads it
back:

```text
co-simulation: part eeprom (eeprom93c46) attached: wired CS=GPIO10 DI=GPIO12 DO=GPIO13 SK=GPIO11; drives GPIO 13; is the SPI device on chip select GPIO 10
I (32) main: Write: Hello World!
I (52) main: Read: Hello World!
I (52) main: Example finished.
```

`--cosim-log events.jsonl` records each instruction as the part saw it and
the two moves of DO around each write. The example's menuconfig choice
between its interrupt and polling ways of reading the busy signal works
either way.
