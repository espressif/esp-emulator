# Virtual Parts

Real firmware rarely runs on a bare chip. It reads a button, talks to a sensor
over I2C, drives a display over SPI. The emulator lets you add those components
to the emulated chip. Each one is a **part**: a small program that behaves like
the real component on the chip's pins. Press a virtual button and the firmware
sees its GPIO go low and runs its interrupt handler, just as with a real button.
Read a virtual sensor and the firmware gets bytes back over I2C, just as from
the real sensor.

Parts run in two places:

- **The command-line emulator.** A JSON *board file* lists the parts and the
  pins they are wired to. You press buttons and change sensor readings from a
  terminal or a script.
- **A web page.** The browser build runs the same parts in Web Workers, with
  buttons on the page.

A part is a WebAssembly file (`.wasm`). You can use the ones that come with the
emulator, or write your own in Rust or any language that compiles to
WebAssembly. The firmware never knows the component is virtual: it uses the
stock ESP-IDF drivers unchanged.

**What works today:** parts that read and drive GPIO pins, I2C devices, SPI
devices, serial (UART) devices, parts that read a pin as PWM, such as a servo,
analog sources, a voltage on an ADC pad, parts on the chip's EN pin that hold
it in reset, such as a watchdog supervisor, audio devices on I2S, camera sensors on DVP
and MIPI CSI, and parts that draw a picture (`part_render`). A part
that drives pins also clocks the chip's pulse counter (PCNT), so a rotary
encoder needs nothing more, and an RMT receiver routed to the pin records
each level it drives for as long as it holds it, as `rmt_receive` decodes
an IR remote.

| Example | What it is | Where |
|---|---|---|
| Push button | a button on a GPIO | [part](../../examples/parts/pushbutton), [board](../../examples/boards/button/README.md) |
| SHT31 | a temperature and humidity sensor on I2C | [part](../../examples/parts/sht31), [board](../../examples/boards/sensor/README.md) |
| TMP123 | a temperature sensor on SPI | [part](../../examples/parts/tmp123), [board](../../examples/boards/thermometer/README.md) |
| 93C46 EEPROM | an SPI EEPROM whose chip select the firmware drives as a GPIO | [part](../../examples/parts/eeprom93c46), [board](../../examples/boards/eeprom/README.md) |
| MH-Z19B | a CO₂ sensor on a UART | [part](../../examples/parts/mhz19b), [board](../../examples/boards/co2/README.md) |
| KY-040 | a rotary encoder with a push switch, counted by the chip's PCNT | [part](../../examples/parts/ky040), [board](../../examples/boards/encoder/README.md) |
| SG90 | a hobby servo read as PWM, from LEDC or MCPWM | [part](../../examples/parts/sg90), [board](../../examples/boards/servo/README.md) |
| Potentiometer | a knob's wiper on an ADC pad | [part](../../examples/parts/potentiometer), [board](../../examples/boards/analog/README.md) |
| NTC thermistor | a thermistor divider on an ADC pad | [part](../../examples/parts/ntc_thermistor), [board](../../examples/boards/analog/README.md) |
| Watchdog | a windowed watchdog supervisor that resets the chip through EN | [part](../../examples/parts/watchdog), [board](../../examples/boards/watchdog/README.md) |
| INMP441, MAX98357A | a microphone and an amplifier on I2S | [microphone](../../examples/parts/inmp441), [amplifier](../../examples/parts/max98357a), [board](../../examples/boards/audio/README.md) |
| VS1838B | an infrared receiver and its remote, each key an NEC frame the chip's RMT times | [part](../../examples/parts/vs1838b), [board](../../examples/boards/ir/README.md) |
| SSD1306 | a 128 x 64 OLED screen on I2C, read back through `part_render` | [part](../../examples/parts/ssd1306), [board](../../examples/boards/oled/README.md) |
| DHT22 | a temperature and humidity sensor on one wire, its reply one timed waveform the firmware measures | [part](../../examples/parts/dht22), [board](../../examples/boards/dht/README.md) |
| WS2812 | an LED strip the chip drives through RMT, its pin read in runs of edges | [part](../../examples/parts/ws2812), [board](../../examples/boards/ws2812/README.md) |
| OV2640 | a camera sensor on DVP (ESP32-S3, P4, S31), which can be shown a picture | [part](../../examples/parts/ov2640), [board](../../examples/boards/camera/README.md) |
| OV5647 | a camera sensor on ESP32-P4's MIPI CSI link | [part](../../examples/parts/ov5647), [board](../../examples/boards/camera-p4/README.md) |

More parts will come from **esp-virtual-parts**, a separate project that will
publish one `.wasm` file per component, plus an `index.json` listing each
part's pin names and the interactions it understands.

## Try it in five minutes

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit has a
`button/`, a `sensor/`, a `thermometer/`, a `co2/`, an `encoder/`, a `servo/`,
an `analog/`, a `watchdog/`, an `audio/`, an `ir/`, an `oled/`, a `dht/`, a `ws2812/`, a `camera/` and a `camera-p4/`
folder. Each holds a board file, the parts and the firmware, ready to run:
ESP32-C3 firmware, ESP32-C6 for the encoder, since the C3 has no pulse
counter, ESP32-S3 for `camera/` and ESP32-P4 for `camera-p4/`.

In the kit's `button/` folder:

```sh
/path/to/esp-emu --chip esp32c3 --firmware gpio_button-esp32c3.bin \
    --cosim-board board.json --control-tcp 127.0.0.1:5556
```

The firmware prints `BUTTON: ready level=1`. In a second terminal, connect to
the emulator and press the button:

```sh
nc 127.0.0.1 5556
part button input press
part button input release
```

The firmware prints `BUTTON: pressed`, then `BUTTON: released`, from its own
GPIO interrupt handler. The [button board README](../../examples/boards/button/README.md)
walks through this in more detail. The sensor and thermometer boards work the
same way, with `warmer` and `cooler` instead of `press` and `release`, the
CO₂ board with `more` and `less`, the encoder board with `cw` and `ccw`, one
detent each, the analog board's knob with `more` and `less`, and the IR board
with its remote's keys, such as `ok`, `left` or a digit. The servo board needs
nothing typed: the firmware turns the servo, and `part servo state` says where
its horn is. On the audio board, `part mic input higher` moves the
microphone's tone up an octave, and `part amp state` says what the amplifier
plays. On the OLED board, `part button input press` redraws the screen, and
`part oled render` reads it back, in hex.

### In the browser

The same button, sensor, thermometer, CO₂ sensor, servo, knob, microphone,
amplifier, IR remote, OLED screen, DHT22, LED strip and cameras run in a web page, with
nothing to install:

- <https://espressif.github.io/esp-emulator/button-demo.html>
- <https://espressif.github.io/esp-emulator/sensor-demo.html>
- <https://espressif.github.io/esp-emulator/thermometer-demo.html>
- <https://espressif.github.io/esp-emulator/co2-demo.html>
- <https://espressif.github.io/esp-emulator/encoder-demo.html>
- <https://espressif.github.io/esp-emulator/servo-demo.html>
- <https://espressif.github.io/esp-emulator/adc-demo.html>
- <https://espressif.github.io/esp-emulator/audio-demo.html>
- <https://espressif.github.io/esp-emulator/ir-demo.html>
- <https://espressif.github.io/esp-emulator/oled-demo.html>
- <https://espressif.github.io/esp-emulator/dht-demo.html>
- <https://espressif.github.io/esp-emulator/ws2812-demo.html>
- <https://espressif.github.io/esp-emulator/camera-demo.html>

Everything runs in your browser. Nothing is uploaded. The log shows the part
attaching, then the firmware's output. Click **Press** and the firmware prints
`BUTTON: pressed`. On the sensor page, **Warmer**, **Cooler**, **Wetter** and
**Drier** change what the part measures, and the next reading follows; on the
thermometer page, **Warmer** and **Cooler**; on the CO₂ page, **More CO₂** and
**Less CO₂**. On the encoder page, **Turn right** and **Turn left** move the
knob one detent, and the firmware's count moves by four. The servo page draws the horn as the firmware turns it. On the knob
page, **Turn up** and **Turn down**. On the audio page, **Higher**, **Lower**,
**Louder** and **Quieter** change the tone the microphone hears; the firmware
reports it once a second and the amplifier shows what it plays. **Listen**
plays what reaches the amplifier's speaker, **Use my microphone** makes the
microphone part hear yours (the browser asks first), and **Tone** goes back
to the tone. On the IR
page, each key of the remote sends one frame, and the firmware prints the
address and command it decoded. On the OLED page, **Press** redraws the
screen, which the page reads back from the part and draws. On the DHT22 page,
**Set** gives the sensor a temperature and a humidity, and the next reads,
twice a second, carry them. The strip page draws each frame the firmware
sends the strip.

To run the pages locally, download `esp-emu-<version>-wasm.tar.gz` from the
release, unpack it, and serve its `www/` folder with the server that comes in
it:

```sh
cd esp-emu-<version>-wasm/www
python3 serve.py 8080
# open http://localhost:8080/button-demo.html
```

`www/` holds the demo firmware and parts as `_button_fw.bin`,
`pushbutton.wasm`, `_sensor_fw.bin`, `sht31.wasm`, `_thermometer_fw.bin`,
`tmp123.wasm`, `_co2_fw.bin`, `mhz19b.wasm`, `_encoder_fw.bin`, `ky040.wasm`,
`_servo_fw.bin`, `sg90.wasm`, `_adc_fw.bin`, `potentiometer.wasm`,
`_audio_fw.bin`, `inmp441.wasm`, `max98357a.wasm`, `_ir_fw.bin`,
`vs1838b.wasm`, `_oled_fw.bin`, `ssd1306.wasm`, `_dht_fw.bin`, `dht22.wasm`,
`_strip_fw.bin`, `ws2812.wasm`, `_camera_fw.bin`, `_camera_p4_fw.bin`,
`ov2640.wasm` and `ov5647.wasm`. Replace them to run your
own; with a file of a demo missing, its page asks for all of them. `serve.py`
tells the browser not to cache, so a part you rebuild and copy in is the one
the next reload runs. The demo pages only know the push button, the SHT31, the
TMP123, the MH-Z19B, the KY-040, the SG90, the potentiometer, the INMP441,
the MAX98357A, the VS1838B, the SSD1306, the DHT22, the WS2812, the OV2640
and the OV5647. Any other
part runs in [a page of your own](#use-parts-in-your-own-page).

## Run parts in the command-line emulator

The release binaries run `.wasm` parts inside the emulator. A board file tells
the emulator which parts are on the board and where each one is wired:

```json
{ "parts": {
    "button": { "module": "pushbutton.wasm", "pins": { "A": 9 } }
} }
```

- The key, `button`, is your name for this placement. You use it in commands.
- `module` is the `.wasm` file, found next to the board file. Leave it out and
  the emulator looks for `<name>.wasm`.
- `pins` maps the part's own pin names to the chip's GPIO numbers. The push
  button calls its signal pin `A`, so `{ "A": 9 }` puts it on GPIO 9. A part
  may take settings there too, such as a camera's `XCLK_HZ` or a panel's `W`
  and `H`: a value is handed to the part as it is, and only a pin the part
  claims is checked against the chip's pads.

Run it with `--cosim-board board.json`. As each part attaches, the log says
how it is wired:

```text
co-simulation: part button (pushbutton) attached: wired A=GPIO9; drives GPIO 9
```

`button` is your name for it and `pushbutton` is the name the part gives
itself.

**Pin names are case-sensitive, and a wrong name is not an error.** A pin the
part does not have is simply left unwired, and the part does nothing. If a
part attaches and then nothing happens, check the names. The attach line
helps: a button that should drive a pin but says `drives no pin` was wired by
a name it does not know.

A part the chip refuses stops the run before the firmware starts, with the
reason: a pin the chip does not have, a pin another part already drives, an
I2C address another part already answers on the same pads, or a chip select
another part is already on.

A part cannot reach your network, your files or anything outside itself. It
is loaded in a sandbox.

### Wire an I2C part

An I2C part is wired by its `SCL` and `SDA` pins. These must be the same GPIOs
the firmware gives its I2C bus:

```json
"sensor": { "module": "sht31.wasm", "pins": { "SCL": 6, "SDA": 5 } }
```

You do not say which I2C port that is. The emulator works it out when the
driver sets the bus up. The log then says `answers I2C 0x44 on GPIO 6/5`.

Some devices let the board choose their address with a strap pin. `addr` sets
it: `"addr": "0x45"` or `"addr": 69` (JSON has no hex numbers, so hex goes in
a string). Without it the part uses its own default.

On a port that has a part, only parts answer. An address no part claims gets a
NACK, so a bus scan finds exactly the board's devices. A port with no part
keeps the emulator's own devices: a built-in test EEPROM at 0x50 and any
`--i2c-dev` device. So on a two-port chip with a part on port 0 only, a scan
of port 1 still finds 0x50.

### Wire an SPI part

An SPI part is wired by its chip select. It must be the GPIO the firmware
gives the device's CS. Clock and data pins may be given too, and are checked
against that bus:

```json
"thermometer": { "module": "tmp123.wasm", "pins": { "CS": 7, "SCK": 6, "SO": 2 } }
```

You do not say which SPI bus or chip-select slot that is. The emulator works
it out when the driver adds the device. The log then says `is the SPI device
on chip select GPIO 7`.

The part sees every transfer on its chip select, writes included, and answers
the reads. It does not matter whether the firmware reads into the controller's
buffer or through DMA. A chip select no part is on reads 0xFF, like a line
nobody drives.

**A chip select the firmware drives as a plain GPIO.** Some drivers set
`spics_io_num = -1` and toggle the chip select themselves. ESP-IDF's
`hd_eeprom` example and the SD card driver `esp_driver_sdspi` do this. Then
no chip-select signal of the controller lands on the pad, and the emulator
cannot tell from the pad which bus the device is on. Such a part names its
clock or data pins in its claim, and the emulator finds the bus from those.
Each transfer then goes to the part whose chip-select pad is at its selecting
level while the transfer is clocked: low, unless the part says
`cs_active_high`. The [93C46 EEPROM](../../examples/parts/eeprom93c46) is such
a part, and the [EEPROM board](../../examples/boards/eeprom/README.md) runs
ESP-IDF's `hd_eeprom` example against it unchanged.

### Wire a serial part

A serial part is wired by its own `TX` and `RX` pins, as its module labels
them. The sensor's `TX` goes to the GPIO the firmware gives its UART's RX,
and the sensor's `RX` to the UART's TX:

```json
"co2": { "module": "mhz19b.wasm", "pins": { "TX": 5, "RX": 4 } }
```

You do not say which UART that is. The emulator works it out when the
driver's `uart_set_pin` runs, through the GPIO matrix or the UART's own pads.
The log then says `is the serial device on GPIO 4/5 (TX/RX from the chip's
end)`: the claim names the pads the way the chip sees them.

The part gets every byte the firmware transmits on that UART, in pieces of up
to 128 bytes, and what it sends back lands in the UART's receive FIFO, 128
bytes at a time, paced by the baud rate the firmware set. Nothing waits: the
firmware is never stopped for the part, and a part that does not answer is a
device that says nothing, which the firmware sees as its own read timeout.

UART0 and UART1 are modelled, on every chip. UART0 is usually the console,
and a part is not wired to the console's UART: the emulator's own output and
input use it. If the firmware's console is on USB-Serial/JTAG
(`--console usb-jtag`, or the firmware selects it) or on a USB CDC-ACM port
(`--console usb-cdc`), UART0 is free for a part, from the moment the console
moves. A UART that `--uart-tcp` or `--uart1-tcp` bridges to a socket is not a
part's either. In both cases the emulator says so once, and wires the part as
soon as the UART is free.

### Wire a PWM part

A part that reads a pin as PWM, such as a servo, a buzzer, a dimmed LED or a
motor driver, is wired by its signal pin:

```json
"servo": { "module": "sg90.wasm", "pins": { "SIG": 4 } }
```

The log says `reads GPIO 4 as PWM`. Whichever block the firmware puts on the
pin, LEDC (`ledc_channel_config`) or an MCPWM generator
(`mcpwm_new_generator`), the part is told the period and the high time on it,
in nanoseconds, whenever they change, and not otherwise: a steady waveform is
one message however long it runs. A hardware fade reaches it step by step,
with no register write, at most one step each millisecond of emulated time and
always the last one. A period of zero means no waveform: the channel stopped,
its timer is held, or the pin carries something else.

Several parts may read one pin. While a PWM block toggles a pin some part reads
this way, the pin's level is not reported to the parts once a tick (it would
only sample the waveform); a part that wants every edge as well also watches
the pin. An MCPWM generator that goes high more than once a period is not one
waveform and is reported as none; its edges are still there for a watcher.

### Wire an analog part

An analog part is wired by its output pin, the pad it puts a voltage on:

```json
"knob": { "module": "potentiometer.wasm", "pins": { "W": 3 } }
```

The pad must be an ADC pad of the chip, one its `adc_channel.h` gives a
channel: GPIO 0-5 on the C3, 1-6 on the C5, 0-6 on the C6, 1-5 on the H2,
16-23 and 49-54 on the P4, 1-20 on the S3, 42-57 on the S31. Any other pad is
refused when the board loads. One source per pad: a pad another part drives,
as a GPIO or as an analog source, is refused.

The log then says `puts a voltage on GPIO 3`. The part sends millivolts, not
counts. The firmware's conversion reads them at
whatever attenuation it chose, so the same part works at 0 dB and at 12 dB.
On the C5, C6 and H2 the code follows the chip's own ADC calibration point,
so `adc_cali_raw_to_voltage` gives the part's voltage back, less the
scheme's own curve correction: a few millivolts. On the C3, the P4 and the
S3 the code is the one IDF's curve fitting reads back as the part's voltage,
within 3 mV, up to the top of each attenuation's range. On the S31 the code is the one its line-fitting scheme reads,
±2000 mV around 0 V. A voltage holds until the part sends another, across a
chip reset, and stays when the part goes; above the 3300 mV supply it is
taken as 3300. A voltage takes effect at the time it is stamped for, to the
nanosecond, so a part may send its voltages ahead of the chip; a signal
sampled at a fixed rate goes as `AnalogWave` pieces, one voltage a period
(see `proto/PART_ABI.md`, "Sending a sampled voltage"). Each conversion reads
the voltage held at its own moment, in continuous mode too: a voltage that
changes during a DMA frame changes inside it. `tools/cosim-tcp.py PORT adc
--pin N --sine OFFSET:AMPLITUDE:HZ` plays a sine, a square or a constant onto
a pad over `--cosim-tcp`.

What an analog part cannot do yet:

- **A differential pair.** A voltage is unsigned and belongs to one pad. On
  the S31, whose channels come as P and N pads, hold each pad's own voltage
  against ground with its own `AnalogDrive`; the firmware reads each side as
  a single-ended channel. The differential bit of a pattern entry is not
  modelled, so a conversion never subtracts one pad from the other.
- **A slope.** A voltage holds until the next one. A ramp or a ripple goes
  as `AnalogWave`, sampled by the part at the rate it chooses; nothing
  interpolates between two voltages.
- **Starting a conversion from another block.** The controller's ETM trigger
  and its ETM events are not modelled, nor are its IIR filters and threshold
  monitors (their registers are stored), its free-running mode (run as
  timer-triggered, said once) and the S31's start delay of about 10 us.
- **Two units at once on the S31** convert on every trigger, each a word, so
  a stream with both units carries twice the conversions a second the
  driver's `sample_freq_hz` names.

### Wire a part to EN (reset)

EN (CHIP_PU) is the chip's reset input. It is not a GPIO, so the board file
names it instead of numbering it:

```json
"wdt": { "module": "watchdog.wasm", "pins": { "WDI": 4, "RESET": "EN" } }
```

The log then says `wired RESET=EN WDI=GPIO4` and `may hold the chip in reset
through EN`. While the part holds EN low:

- The chip runs nothing, and every pin reads released, as on a chip in reset.
- Emulated time goes on, for this part and every other on the board. A part
  can queue the release ahead, so a 200 ms reset pulse is 200 ms on the
  chip's clock however fast the emulator runs.
- When the part lets go, the chip boots as from power-on:
  `esp_reset_reason()` is `ESP_RST_POWERON`, and nothing in RAM survives.

Several parts may be wired to EN, a supervisor and a reset button for
instance. The line is low while any of them holds it low, and the host's
`en low` (below) counts as one more. A part that fails or leaves lets go.
Every part is told when EN moves, whoever moved it, so one that only watches
can follow it.

### Wire an audio part

An audio part is wired by its data pins, as its module labels them: a
microphone's data output to the GPIO the firmware gives its I2S unit's data
input, an amplifier's data input to the unit's data output. The bit clock and
the word select need no wire:

```json
"mic": { "module": "inmp441.wasm", "pins": { "SD": 7 } },
"amp": { "module": "max98357a.wasm", "pins": { "DIN": 6 } }
```

You do not say which I2S unit that is. The emulator reads it from the GPIO
matrix once the driver has routed the pads, and again whenever it routes them
anew. The log says `is the audio device on I2S data-in GPIO 7 (from the
chip's end)`. Once the firmware starts a direction, the part on its data pad is
told the rate, the slots in a frame and the width of a slot. An amplifier then
gets every sample the unit clocks out, in pieces of at most 4096 bytes. A
microphone sends its samples from its polls and keeps them a little ahead of
emulated time: the receiver takes them at the rate the firmware programmed,
and takes silence after 20 ms without any. One part per data pad and
direction; a codec names both pads. Nothing waits on a part: the firmware is
never stopped for one. A pad the firmware never puts I2S data on is named when
the run ends, beside the pads it did use.

### Wire a camera part

A camera sensor is wired by its pins, as its module labels them: on a DVP
bus, its 8-bit data bus, its pixel clock, VSYNC and HREF to the pads the
firmware gives its camera controller, XCLK where the chip clocks it, and its
SCCB pins to the firmware's I2C pads. A MIPI CSI sensor's lanes are the
ESP32-P4's own pads, so only SCCB is wired:

```json
"camera": { "module": "ov2640.wasm", "pins": { "D0": 4, "D1": 5, "D2": 6, "D3": 7,
    "D4": 8, "D5": 9, "D6": 10, "D7": 11, "PCLK": 12, "VSYNC": 13, "HREF": 14,
    "XCLK": 15, "SIOC": 16, "SIOD": 17 } },
"csi": { "module": "ov5647.wasm", "pins": { "SIOC": 16, "SIOD": 17 } }
```

The firmware programs the sensor over SCCB, as its driver does, and the part
takes its picture's size and format from those registers. A DVP sensor is on
the receiver's bus once the driver routes the receiver's pixel clock to its
pad. The attach line, `is the DVP camera sensor, pixel clock on GPIO 12`,
says only what it claims; once it is on the bus it is sent a `DvpFormat`
(`--cosim-log`), and its frames are received while that says `capturing`. It is told whether the receiver captures and sends its frames at its own rate, each
in pieces its buffer holds. Nothing waits on a part: the firmware receives
what arrives while it captures, as from a sensor on a board. The
[camera board](../../examples/boards/camera/README.md) (ESP32-S3) and the
[ESP32-P4 camera board](../../examples/boards/camera-p4/README.md) run
[`ov2640`](../../examples/parts/ov2640/README.md) and
[`ov5647`](../../examples/parts/ov5647/README.md), which send colour bars
until they are shown a picture: `part camera video FILE` on the control
channel or `--part-video camera=SRC`, or a page's webcam or test card through
the worker's `video_in`
([Use parts in your own page](#use-parts-in-your-own-page)). A sensor in JPEG
mode sends only the JPEG it was shown.

### Built-in devices and wires

The board file can also place the emulator's own simple device models and
wires, the same ones `--i2c-dev` and `--rmt-loopback` add from the command
line. One file then describes the whole board:

```json
{ "parts": {
    "button": { "module": "pushbutton.wasm", "pins": { "A": 9 } }
  },
  "devices": {
    "expander": { "type": "regfile", "i2c": 1, "addr": "0x20" },
    "touch":    { "type": "regfile", "i2c": 1, "addr": "0x5a", "width": 2, "fill": 0,
                  "regs": { "0xd006": "0xab" } }
  },
  "wires": [ { "type": "rmt-loopback", "tx": 0, "rx": 2 } ]
}
```

- A `regfile` device is the register file of
  [`--i2c-dev`](CLI.md#i2c-devices): on IDF port `i2c` at `addr`, with the
  same `width`, `fill` and preloaded `regs`. It acknowledges and reads back
  what was written, and nothing more. That is enough to get firmware past the
  init of an IO expander, a touch controller or a PMIC.
- An `sdcard` device is the SD card of [`--sd`](CLI.md#sd-card): `image` is
  the card image, with optional `read_only` and `slot`, or `cs` for a card on
  an SPI bus, and `cd` for its card-detect pad. The image is a file
  in the board file's directory or below it: the firmware writes into it, so
  a board file cannot name a file elsewhere on the host.
- An `rmt-loopback` wire feeds RMT TX channel `tx` into RX channel `rx`, as
  `--rmt-loopback TX:RX` does. Channel numbers are the TRM's.
- Numbers may be JSON numbers or `"0x"` strings.
- A board with only devices and wires, and no part, is fine.
- A device on a port a part is wired to is not asked there. Parts win.
- A port or channel the chip does not have stops the run before the firmware
  starts, naming the entry.

### Other device models over TCP

A device model that is not a `.wasm` part, such as a Python script or another
simulator, can join the same board over `--cosim-tcp`. See
[Co-simulation over TCP](HOST-INTERFACES.md#co-simulation-over-tcp). It sits
beside the board's parts, and a pin one of them claims is refused to the other.

## Drive the parts while the firmware runs

With `--control-tcp HOST:PORT` the emulator takes commands over a socket, one
per line, one answer per line. See
[Host Control Channel](HOST-INTERFACES.md#host-control-channel) for the
channel itself. The `part` commands are:

| Command | What it does |
|---|---|
| `part list` | names every part on the board: `ok button sensor` |
| `part NAME input KIND` | an interaction, such as `press` or `warmer`. The part defines the names. Answers `ok` even for a name the part does not have, which it ignores |
| `part NAME input KIND after MS` | the same, MS milliseconds of emulated time from now |
| `part NAME input KIND at MS` | the same, at emulated time MS |
| `part NAME input set:QUANTITY=VALUE` | sets a measured value directly: `part sensor input set:temperature=30.5` |
| `part NAME state` | what the part shows: `ok pressed=false` for a button. A part that shows nothing answers `err: ... shows nothing` |
| `part NAME render` | what the part draws that is new since it was last asked, in hex: `ok 0ff0...`, in the layout the part states. `ok` alone when nothing is new; a part that draws nothing answers `err: ... draws nothing` |
| `part NAME video-format` | what a camera part asks to be shown: `ok 320 240 JPEG 15` (width, height, fourcc, pictures a second). `ok` alone when it wants nothing now; a part that is shown nothing answers `err: ... is shown nothing` |
| `part NAME video FILE` | shows the part the picture in FILE, whole, at the size `video-format` asked for: a JPEG file for a JPEG mode, else RGB888, which the part makes into its own layout. `ok` when it kept it, else `err:` and why not (another size, not a JPEG, a mode the firmware has left) |
| `part clock` | the emulated time, in milliseconds |
| `part pending` | how many scheduled inputs have not been handed over yet |

From a script:

```sh
printf 'part button input press\n' | nc -N 127.0.0.1 5556
```

On macOS leave out `-N`.

**Timing.** Emulated time is the chip's clock, not your computer's. A `3000 ms`
hold lasts three seconds on the chip however fast or slow the emulator runs.
A scheduled input lands within 1 ms of emulated time after the time it names.
With a TAP network (`--net tap`, which Linux tries first when `--net` is not
given) or `--ble-hci hci0`, an idle chip picks up the input one idle step
later, which can be up to about 6 ms at 160 MHz. A time already past is handed
over at once.

**Chip resets.** When the firmware or a `reset` command resets the chip, the
board does not reset. A button held across the reset is still held, and a
sensor keeps its settings, as on a real board. Scheduled inputs still waiting
are dropped, since their times belong to the run before the reset. Each part is
told about the reset and the pin levels it left.

A reset while EN is held low, by a part or by `en low`, leaves the chip in
reset: EN belongs to the board, and the chip boots when EN is let go. A
release a part had queued is kept.

**At exit** the emulator reports how long the parts took, any part that
failed, and any wiring the firmware never used: an I2C part whose pads never
got a bus, an SPI part whose chip-select pad never got a chip select, a
serial part whose pads never got a UART line, or an audio part whose data
pad never got an I2S line, with the pads the firmware did use. It also reports the worst delay of any
scheduled input over the run.

### Record what crossed the boundary

`--cosim-log events.jsonl` writes every message between the chip and the parts
as it happens, one JSON object per line:

```text
{"dir":"from_part","msg":{"GpioDrive":{"level":false,"pin":9}},"part":"button","t_ns":3125656437}
```

- `t_ns` is emulated time in nanoseconds.
- `dir` is `to_part` or `from_part`.
- `part` is the part's name, or `*` for a message every part gets.
- `msg` is the message, keyed by its kind as named in `proto/esp_cosim.proto`.

The kinds you will see:

| Kind | Meaning |
|---|---|
| `GpioLevel` | the chip moved a pin. One per transition, stamped with the transition's own time, so a pulse inside one peripheral tick is two of them |
| `GpioDrive` | a part drove a pin. Not echoed back to the part |
| `BusRequest` | an I2C transaction (`write` is the bytes sent, `read_len` how many are read back) or an SPI transfer (`tx` the data written, `rx_len` how many bytes are read back, then the framing: command and address phases, line widths, and the pad on each data line) |
| `BusReply` | the part's answer. An SPI write has no answer |
| `UartTx` | bytes the firmware transmitted on a part's UART (`port`, `data`), up to 128 a message |
| `UartRx` | bytes a serial part sent the firmware |
| `PwmState` | the waveform on a pin a part reads as PWM (`pin`, `period_ns`, `high_ns`), when it changes |
| `AnalogDrive` | the voltage an analog part holds on a pad (`pin`, `millivolts`) |
| `AnalogWave` | voltages an analog part sends ahead, one every `period_ns` from the message's time (`pin`, `period_ns`, `millivolts`) |
| `I2sFormat` | the rate, slots and slot width of one I2S direction, sent to the part on its data pad when the firmware starts it and when it changes (`receive` says which direction) |
| `I2sOut`, `I2sIn` | samples the chip clocked out to a part, and samples a part supplied. Left out unless `--cosim-log-filter` names them: there is one of each per DMA buffer |
| `DvpFormat`, `CsiFormat` | what the camera receiver looks like to the sensor part on it (`capturing`, `session`, and the XCLK or the lanes), when it changes |
| `CamFrame` | a piece of a frame a camera part sent (`end_of_frame` on the last, with `width`, `height` and `fourcc`). Left out unless `--cosim-log-filter` names it: each holds up to a buffer of pixels |
| `Register`, `RegisterAck` | the part attaching, and the emulator accepting it |
| `ChipReset` | the chip was reset |
| `EnDrive` | a part pulled the chip's EN pin low (`level` false) or let it go |
| `EnLevel` | EN moved, whoever moved it: told to every part |
| `PulseDrive` | a waveform a part played into a pin (`pin`, `first_level`, `widths_ns`) |
| `GpioEdges` | a run of edges of a pin a part reads as edges (`pin`, `first_level`, `widths_ns`), up to 512 widths |
| `ClockSync` | the heartbeat, once per millisecond. Left out unless `--cosim-log-filter` names it |

`--cosim-log-filter KINDS` keeps only the kinds named, comma-separated. Fields
ending in `_pins` or `_levels` are bit masks, bit N for GPIO N. `line_pins` is
a list of pad numbers, where 4294967295 means a line on no pad.

## Make your own part

A part is a WebAssembly module that imports nothing and exports a fixed set of
functions. The emulator calls them to tell the part what the pins are doing,
to send it bus transactions, and to ask what it wants to do. The functions
are listed in [`proto/PART_ABI.md`](../../proto/PART_ABI.md), and the messages
in [`proto/esp_cosim.proto`](../../proto/esp_cosim.proto). Any language that
compiles to WebAssembly works.

In Rust, the [`esp-part`](../../esp-part/README.md) crate does the WebAssembly
side for you. You write a type that describes your component and implements
its `Device` trait, and add one line of `export_part!`. The component knows
nothing about WebAssembly. Here is a complete LED:

```rust
use esp_part::prelude::*;

#[derive(Default)]
struct Led { pin: Option<u32>, lit: bool }

impl Device for Led {
    const NAME: &'static str = "led";
    fn configure(&mut self, config: &PartConfig) { self.pin = config.pins.get("A").copied(); }
    fn claims(&self) -> Vec<Claim> { Vec::new() }   // it drives nothing
    fn on_pins(&mut self, levels: u64, _now_ns: u64) {
        if let Some(pin) = self.pin { self.lit = levels >> pin & 1 == 0; }   // active low
    }
    fn state(&self) -> Option<State> { Some(State::new().set("lit", self.lit)) }
}

esp_part::export_part!(Led);
```

### Step by step

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

1. **Copy an example.** Start from
   [`examples/parts/pushbutton`](../../examples/parts/pushbutton), the
   simplest part. Copy the directory without its `target/` folder. If you keep
   the copy outside this repository, copy the `esp-part` folder beside it,
   point `esp-part = { path = ... }` in `Cargo.toml` at it, and set
   `ESP_COSIM_PROTO_DIR` to a folder holding a copy of `proto/esp_cosim.proto`
   when you build.

2. **Name it.** In `Cargo.toml`, set `name` to your part's name. The build
   produces `target/wasm32-unknown-unknown/release/<name>.wasm`, with any `-`
   in the name turned into `_`.

3. **Describe your component** in `src/lib.rs`. Replace `PushButton` with
   your device. Decide five things:

   | Decide | Where | Example |
   |---|---|---|
   | its name | `NAME` | `"pushbutton"` |
   | its pin names, which the board file wires | `configure` | `A` |
   | what it drives or answers for | `claims` | a GPIO, an I2C address, an SPI chip select |
   | what people can do to it | `on_input` | `press`, `release`, `warmer` |
   | what it shows | `state` | `pressed=false` |

   [`esp-part`'s README](../../esp-part/README.md) lists every method of
   `Device` and which kinds of part need it. Two things that trip people up:
   a pin no part drives reads high, so a device that rests low must drive its
   resting level from its first `poll`; and `on_input` is not told the time,
   so a device that acts some time after an interaction notes it there and
   starts the clock in the next `poll`. The motion sensor in
   [`PART_ABI.md`](../../proto/PART_ABI.md#two-ways-to-act-at-a-time-and-when-to-use-each)
   shows both.

4. **Build it.**

   ```sh
   cargo build --release --target wasm32-unknown-unknown
   ```

5. **Test it** with `cargo test`. The device also runs as plain Rust on your
   computer, so its tests need no emulator. This is also the place to look
   inside a part: a part has no console, and `println!` in one prints nowhere.

6. **Put it on a board.** Write a board file that wires its pins, as
   [Run parts in the command-line emulator](#run-parts-in-the-command-line-emulator)
   shows, and run the emulator with `--cosim-board`. Or run it in
   [a page of your own](#use-parts-in-your-own-page).

### Add a bus

- **An I2C device** claims an address on its `SCL` and `SDA` pins, sets
  `ON_BUS = true`, and answers each transaction in `on_bus`. The emulator joins
  the pieces a driver splits a write into, so a register read is one call with
  the register in `write` and the length in `read_len`. A read a driver splits
  arrives as several calls that `continue` the first.
  [`examples/parts/sht31`](../../examples/parts/sht31) is a complete I2C part.
- **An SPI device** claims its chip select, sets `ON_BUS = true`, and answers
  the reads in `on_bus`. It returns nothing for a transfer that reads nothing.
  [`examples/parts/tmp123`](../../examples/parts/tmp123) is a complete SPI
  part.
- **An SPI device on a GPIO chip select** also names its clock or data pins in
  its claim, and may say `cs_active_high`.
  [`examples/parts/eeprom93c46`](../../examples/parts/eeprom93c46) is one. It
  also drives a pin back, its busy signal, timed on the emulated clock.
- **A serial device** claims its line with `serial(tx, rx)`, by its own TX and
  RX pins, sets `ON_MESSAGE = true`, takes what the chip sends in `on_uart`, and
  answers from `poll` with `transmit(bytes, now)`. The bytes come in pieces: a
  command may arrive over two calls, so collect them until it is whole.
  [`examples/parts/mhz19b`](../../examples/parts/mhz19b) is a complete serial
  part.
- **A part read as PWM** claims its pin with `reads_pwm(pins)`, sets
  `ON_MESSAGE = true`, and takes the period and high time in `on_pwm`. It is
  told only on a change, so it keeps what it was told; a period of 0 is no
  waveform, and the pin's level is then the one `on_pins` reports.
  [`examples/parts/sg90`](../../examples/parts/sg90) is a complete PWM part.

- **An analog source** claims its pad with `analog(pin)` and, from `poll`,
  holds a voltage there with `hold(pin, millivolts, now)` when it changes, or
  sends a run of voltages one period apart with
  `wave(pin, period_ns, millivolts, now)`. It needs no handler: the chip
  reads the pad when the firmware converts.
  [`examples/parts/potentiometer`](../../examples/parts/potentiometer) is a
  complete analog part, and
  [`examples/parts/ntc_thermistor`](../../examples/parts/ntc_thermistor) one
  with a model behind its voltage, which warms toward a temperature it is
  set to and sends each change as one run.

- **An audio device** claims its data pins with `i2s(din, dout)`, by its own
  data input and output, sets `ON_MESSAGE = true`, learns each direction's
  layout in `on_i2s_format`, hears what the chip plays in `on_i2s_out`, and
  supplies what the chip records from `poll` with `samples(pcm, now)`.
  `audio::Frame` reads and writes one slot of a frame.
  [`examples/parts/max98357a`](../../examples/parts/max98357a) listens, and
  [`examples/parts/inmp441`](../../examples/parts/inmp441) supplies.

- **A part that makes a sound or hears one**, for a person: an amplifier's
  speaker, a microphone, adds `sounds` or `hears` to its `export_part!` line
  and implements `audio_out`, which writes what it makes into the buffer it
  is given, `audio_in`, which takes what somebody plays at it, and
  `audio_rate`. The samples are mono 16-bit at the part's own rate, whatever
  the chip's frame looks like.

- **A part that draws**, such as a lamp or a screen, returns its picture from
  `render` as bytes in a layout it states, and `None` when nothing is new
  since the last call, so a still screen costs nothing to ask about.
  `part NAME render` and a page's `render` message read it; whoever draws
  keeps what it drew. [`examples/parts/ssd1306`](../../examples/parts/ssd1306)
  is a complete part that draws.

Every example part shows what it is set to through `state`, so
`part NAME state` and a web page can read it.

### Time inside a part

A part never sleeps or blocks. It is told the emulated time, `now_ns`, and
says what should happen and when. The usual way to act later, such as a
sensor releasing its pin two seconds after a trigger, is to keep the deadline
in the part's own state and act when a later `poll` sees the clock pass it.
That lands within one heartbeat, a millisecond.
[`PART_ABI.md`](../../proto/PART_ABI.md#two-ways-to-act-at-a-time-and-when-to-use-each)
shows the code and explains the second, finer-grained way.

A part is polled on each heartbeat: once a millisecond while the chip runs,
and once per sleep while the chip waits for its next tick, which with
ESP-IDF's default 100 Hz tick and a firmware that mostly sleeps is 2.5 ms
apart on P4 and up to 10 ms apart on H2. Each time, `poll` is asked again
until it has nothing to say, up to 256 answers, so a part hands over all it
owes at once. Edges it stamps ahead land at their times, each to within a
peripheral tick: a whole train of them, such as a frame of the
[VS1838B](../../examples/parts/vs1838b)'s IR remote, goes over in one poll
and plays out as stamped, as long as it is stamped far enough ahead to reach
the emulator before its first edge is due (a heartbeat or two natively, more
from a browser worker). The emulator holds at most 4096 events stamped ahead,
for all parts together, refusing the rest and counting them in
`ClockSync.scheduled_dropped`.

A part whose device speaks in timing on one wire returns the whole waveform
as one message, `play(pin, first_level, widths_ns, at)`, a `PulseDrive`
([`PART_ABI.md`](../../proto/PART_ABI.md#playing-a-waveform)). That is the
form for an answer inside a window the firmware opened, such as the
[DHT22](../../examples/parts/dht22)'s reply 30 µs after the start pulse: a
reply that reaches the emulator late, as one from a browser worker does,
starts at once and keeps every width, where edges sent one by one would fire
together and read short.

The other way round, a part that reads the chip's waveform on a pin, such
as the [WS2812](../../examples/parts/ws2812) strip's thousands of edges a
frame, claims it with `reads_edges(pins)` and is handed the edges in runs
(`Device::on_edges`: a first level and the widths after it, a `GpioEdges`),
a few messages a frame rather than one per edge, at most a heartbeat after
they happen. A part that must react to an edge sooner watches the pin
(`watches(pins)`) instead and is told each edge as it comes.

A part that stamps each edge with the time it is polled, as the
[KY-040](../../examples/parts/ky040) does with its 250 µs spacing, puts one
edge on the pins a heartbeat: 25 to 100 detents a second, more than a hand
turns. It queues what it owes, so a quick run of turns plays out a little
slower than it was clicked and loses nothing.

While a part answers an I2C transaction or an SPI read, the chip runs no
instructions. The firmware sees the answer after the time the bus takes to
carry it, however long the part itself took. So a part may take as long as it
needs, but it must not add bus time itself.

## Use parts in your own page

Any part runs in a page of your own. The page lives in the unpacked `www/`
folder of the release's `esp-emu-<version>-wasm.tar.gz`, beside the files it
uses:

- `pkg/esp_emu.js` (with `pkg/esp_emu_bg.wasm`): the emulator.
- `cosim.js`: `CosimPump`, which connects the emulator to each part and runs
  the firmware.
- `part-worker.js`: runs one part in a Web Worker. Use one worker per part.
- `vendor/esp_cosim_pb.js`: the contract's JavaScript bindings, which
  `part-worker.js` imports. Keep it at that path beside `part-worker.js`.

Save this as `my-part.html` in that `www/` folder, put the firmware and your
`.wasm` beside it, serve the folder with `python3 serve.py 8080` and open
<http://localhost:8080/my-part.html>. The hosted site cannot run it: it serves
only the files a release ships.

```html
<!doctype html>
<meta charset="utf-8">
<title>My part</title>
<p id="inputs"></p>
<pre id="log"></pre>
<script type="module">
import init, { WasmEmulator } from "./pkg/esp_emu.js";
import { CosimPump } from "./cosim.js";

// What to change for your part.
const FIRMWARE = "./merged_flash.bin";  // test_apps/gpio_button, built for esp32c3
const PART = "./pushbutton.wasm";       // your part, as cargo built it
const PINS = { A: 9 };                  // the part's own pin names -> GPIO numbers
const INPUTS = ["press", "release"];    // the interactions it understands

const out = document.getElementById("log");
const log = (text) => { out.textContent += text.endsWith("\n") ? text : text + "\n"; };

await init();
const emu = new WasmEmulator("esp32c3");
emu.load_default_rom();
const fw = await fetch(FIRMWARE, { cache: "no-cache" });
if (!fw.ok) log(`could not fetch ${FIRMWARE}: HTTP ${fw.status}`);
emu.load_firmware(new Uint8Array(await fw.arrayBuffer()));  // a Uint8Array, not an ArrayBuffer

const worker = new Worker("./part-worker.js", { type: "module" });
worker.onmessage = (e) => log("part: " + JSON.stringify(e.data));
const chan = new MessageChannel();
const pump = new CosimPump(emu, { mypart: { port: chan.port1, worker } });
worker.postMessage({ type: "start", port: chan.port2, module: PART,
                     config: { pins: PINS } }, [chan.port2]);

// The chip is held at reset until the part attaches.
if (!(await pump.waitForDevice())) {
  log("no part attached within 5 s");
} else {
  pump.start(log, (err) => log("emulator error: " + err));
  for (const kind of INPUTS) {
    const b = document.createElement("button");
    b.textContent = kind;
    b.onclick = () => worker.postMessage({ type: "input", kind });
    document.getElementById("inputs").append(b);
  }
}
</script>
```

The log shows the part loading and the emulator accepting it (`part:` lines),
then the firmware's output. Each button sends its interaction to the part. To
run a different part, change `PART` to its `.wasm`, `PINS` to its pin names
and `INPUTS` to the interactions it understands. An I2C part takes
`config: { pins: PINS, addr: 0x45 }` when the board straps its address.

Several parts are several workers, each under its own name in the object
passed to `CosimPump`. Two parts may not drive the same pin, answer the same
I2C address on the same pads, or sit on the same chip-select pad.

### Reference

**`CosimPump`** (`cosim.js`):

| Call | What it does |
|---|---|
| `new CosimPump(emu, ports, quantumUs = 1000)` | Attaches the co-simulation boundary to `emu`. `ports` maps each part's name to its `MessagePort`, or to `{ port, worker }` with the `Worker` running the part, which lets `start` stop a part that never answers (one bare entry is named `part`). `quantumUs` is the heartbeat, in emulated microseconds. |
| `waitForDevice(timeoutMs = 5000)` | Holds the firmware at reset until a part has registered. Resolves to `true`, or `false` on timeout. |
| `pumpUntil(done, timeoutMs = 8000)` | The same, until `done()` returns true: for a page waiting on several parts. |
| `start(onOutput, onError, hooks)` | Runs the firmware, paced to the wall clock. `hooks.onBatch()`, if given, runs after every batch: where a page asks its parts for the sound they make. `onOutput` gets the UART output, one or more whole lines per call. `onError` is called once if a batch throws, if the machine faults (a CPU fault or an intercepted panic, the fault report as the error's message), or if a part has not answered a bus transaction after 5 s, and the loop stops; in the second case every `Worker` the constructor was given is terminated first, so a spinning part does not go on taking a core. A restart the firmware asks for is performed; the parts stay attached. |
| `stop()` | Stops the loop and flushes the last unfinished line to `onOutput`. |
| `drain()` | Delivers every waiting frame to the parts. Only for a page that runs its own loop with `emu.run_batch` instead of `start`; such a page also decides how long to wait while `emu.cosim_awaiting_device()` says a part is answering. |

**`part-worker.js`** messages:

| Direction | Message | Meaning |
|---|---|---|
| page → worker | `{ type: "start", port, module, config }` | Load the part. `port` is the worker's end of the part's `MessageChannel` (transfer it); `module` is the `.wasm` URL or its bytes as an `ArrayBuffer`; `config` is `{ pins: { NAME: gpio }, addr, enPins: [NAME] }`, with `addr` only for an I2C part that takes one and `enPins` naming the part's pins wired to the chip's EN. An SPI or serial part is wired by its pins alone. |
| page → worker | `{ type: "input", kind }` | Somebody interacted with the part: `kind` is the interaction's name, such as `"press"`. |
| page → worker | `{ type: "state" }` | Somebody is looking at the part: ask what it shows. |
| worker → page | `{ type: "state", values }` | What the part shows, as `{ name: "value" }` in the part's own words, or `null` for a part that shows nothing. |
| page → worker | `{ type: "render" }` | Something is drawing the part: ask what it draws. |
| worker → page | `{ type: "rendered", bytes }` | What the part draws that is new since it was last asked, as a `Uint8Array` in the layout the part states, or `null` when nothing is new or the part draws nothing. The page keeps what it drew. |
| page → worker | `{ type: "video_format" }` | Something is about to show the part a picture: ask what it wants. |
| worker → page | `{ type: "video_format", format, error? }` | What the part asks to be shown, `{ width, height, fourcc, fps }`, or `null` when it wants nothing now or is shown nothing; `null` with an `error` when it asked for a picture that is empty or more than 64 MiB of RGB888. Ask before each picture: a camera's firmware reprograms it. |
| page → worker | `{ type: "video_in", buf }` | A picture, whole, as an `ArrayBuffer` of the size `format` asked for: a JPEG file for a JPEG mode, else RGB888 (a canvas's RGBA without its alpha), which the part makes into its own layout. |
| worker → page | `{ type: "video_in", ok, error }` | Whether the part kept it; `error` says why not, in the native host's words: a picture larger than its buffer, or the part's own reason. Send the next picture after this answer. |
| page → worker | `{ type: "audio_out" }` | Somebody is listening to the part: ask for the sound it makes. |
| worker → page | `{ type: "audio", bytes }` | The answer to each `audio_out`, in order: what the part made since it was last asked, mono 16-bit little-endian samples in a `Uint8Array`, at most its buffer's worth, empty for nothing new or a part that makes no sound. A full answer is a reason to ask again at once. |
| page → worker | `{ type: "audio_in", buf }` | Somebody makes a sound at the part: `buf`, an `ArrayBuffer` of mono 16-bit little-endian samples at the rate it hears at. Dropped for a part that hears nothing. |
| worker → page | `{ type: "audio_rate", sounds, hears }` | The samples a second the part makes its sound at and hears at, 0 while it does not know; sent when either moves. |
| worker → page | `{ type: "loaded", name, audio, video }` | The module loaded; `name` is the name the part gives itself, `audio` is `{ hears, sounds }`: whether it hears a sound and makes one, and `video.sees` whether it is shown pictures. |
| worker → page | `{ type: "ready", chip }` | The emulator accepted the part. |
| worker → page | `{ type: "error", text }` | The part was refused, failed to load, or crashed; `text` says which. |

`CosimPump` uses seven calls on the emulator itself: `cosim_attach`,
`cosim_service`, `cosim_has_consumer`, `cosim_take_outbound`,
`cosim_push_inbound`, `cosim_awaiting_device` and `cosim_awaited`. A page using
`CosimPump` does not call them directly.

## Which versions work together

Two version numbers decide whether a part and an emulator can work together.
Both are declared in `proto/esp_cosim.proto`:

| Number | Checked | Moves when |
|---|---|---|
| `ProtoVersion` | when the part registers with the emulator | a message changes in an incompatible way |
| `PartAbiVersion` | when the module is loaded, by `part-worker.js` or the command-line emulator | the functions a part exports change |

A mismatch is refused with a message saying which number differs. A part built
for an older emulator keeps working as long as neither number moved.

Neither number moves for an addition. A part that uses something newer than
the emulator it runs in is not refused: the emulator logs a message it does
not take and the part's request does nothing. EN wiring is such an addition,
new after 1.0.0: on 1.0.0 a board file with `"EN"` is refused as not a number.

## When something goes wrong

| Message | Meaning |
|---|---|
| `part is missing part_configure, ...` | the file is not a part, or was built for a different part ABI |
| `part speaks ABI N; this host speaks M` | the part and this emulator release were built for different part ABIs; get matching versions |
| `refused: protocol vN != emulator vM ...` | the part and the emulator speak different contract versions |
| `refused: GPIO N is already driven by "NAME"` | two parts are wired to the same pin; NAME is the part that has it |
| `refused: GPIO N is beyond this chip's M pins` | the part is wired to a pin this chip does not have |
| `refused: GPIO N is not a pad this chip has` | the part is wired to a gap in the chip's pads: ESP32-S3 has no GPIO 22-25, ESP32-S31 no GPIO 29 or 41 |
| `claims I2C 0xNN but exports no part_on_bus` (in the browser: `claims an I2C address ...`) | an I2C part built without its bus handler |
| `claims the SPI chip select on GPIO N but exports no part_on_bus` (in the browser: `claims an SPI chip select ...`) | the same, for an SPI part |
| `reads GPIO N as edges but exports no part_on_message` | a part that reads edges built without its `part_on_message` export |
| `reads GPIO N as edges with a N-byte buffer; such a part's buffer holds at least 2581` | a run of 512 widths does not fit; make the part's buffer larger |
| `claims the clock, which a .wasm part cannot hold yet` / `claims to be an SPI master, which a .wasm part cannot be yet` | only a consumer on `--cosim-tcp` can hold the clock or be an SPI master |
| `exports part_video_format but no part_video_in` (or the reverse) | the two video exports come as a pair |
| `ignoring waveform on GPIO N from NAME: N widths, the limit is 512` | a part played a waveform longer than one message carries; send it in pieces |
| `claims the serial line on GPIO T/R but exports no part_on_message` | a serial part built without its `part_on_message` export; a part made with `esp-part` always has one |
| `reads GPIO N as PWM but exports no part_on_message` | a PWM part built without its `part_on_message` export; a part made with `esp-part` always has one |
| `a PWM state for GPIO N to NAME was refused; it is sent again ...` | the link to the part was full; the waveform is sent again a millisecond later. Logged at the first and then at each doubling of the count |
| `refused: a UART claim has no tx_pin ...` | the serial part is wired without its `TX` or `RX`; pin names are case-sensitive |
| `refused: GPIO N is already on the serial line of "NAME" ...` | two serial parts are wired to one pin; NAME is the part that has it |
| `UART0 is the console, so NAME on GPIO T/R is not wired to it while it is` | the firmware put the part's pins on the console's UART; it is wired once the console moves to USB, or never |
| `UART1 is a host bridge's (--uart-tcp, --uart1-tcp), so ...` | a socket bridge holds that UART; drop the bridge or move the part |
| `refused: GPIO N has no ADC channel on this chip` | an analog part wired to a pad no ADC channel is bonded to |
| `refused: GPIO N is already driven by "NAME" — one source per analog pad` | an analog part on a pad another part, NAME, drives |
| `ignoring a voltage on GPIO N from NAME — it claims no analog pad there` | the part sent an `AnalogDrive` for a pad it did not claim |
| `GPIO N driven to M mV, above the pad's 3300 mV supply: taken as 3300` | an analog part's voltage is above the supply |
| `refused: an SPI claim names no chip select (cs_pin) ...` | the SPI part is wired without its `CS`; pin names are case-sensitive |
| `refused: SPI chip select GPIO N is already claimed by "NAME" ...` | two parts are wired to one chip-select pad; NAME is the part that has it |
| `a part did not answer within 5 s: an SPI read on bus B CS S, from NAME` | the part failed while answering a read, as for I2C below, with `BUS_OP_SPI` |
| `A device model did not answer: an SPI read on bus B CS S, from NAME, which could not be asked ...` | the same, from the command-line emulator, which stops at once |
| `NAME answered N bytes to a M-byte read on SPI bus B CS S; ...` | the part's answer is not the length the firmware reads: the rest reads 0xFF, or the extra is dropped. Logged at the first and then at each doubling of the count |
| `NAME: the board gives it addr N (0xNN), an I2C address, but it claims none ...` | `addr` on a part that is no I2C device; it is ignored |
| `refused: I2C 0xNN has no scl_pin ...` (or `sda_pin`) | the part is wired without `SCL` or `SDA`; the names are upper case |
| `refused: I2C 0xNN on GPIO A/B is already claimed by "NAME" ...` | two parts are wired as the same device on the same bus; NAME is the one that has it |
| `a part did not answer within 5 s: I2C 0xNN on port P, claimed by NAME` | the part failed while answering a transaction: a trap, a reply that does not decode or does not quote the request's `correlation_id` and `BUS_OP_I2C`, or a request too big for its buffer. The part's own error comes first |
| `A device model did not answer: I2C 0xNN on port P, claimed by NAME, which could not be asked ...` | the same, from the command-line emulator, which stops at once. The part's error is logged before it |
| `NAME claims I2C 0xNN on GPIO A/B, but the firmware never put an I2C bus on ...` | logged at the end of a command-line run: the board's `SCL`/`SDA` are not the pads the firmware gave its bus, which the message lists. The firmware found no such device |
| `NAME is the SPI device on GPIO N, but the firmware never put a chip select on it ...` | logged at the end of a command-line run: the board's `CS` is not the pad the firmware gave the device's chip select; the message lists the ones it did use. Its reads got 0xFF. Or the firmware drives that CS as a plain GPIO and the part's claim names no clock or data pad to tell its bus by |
| `an SPI read on bus B CS S went to no part: NAME is the device on GPIO N, which carries no chip select ...` | logged once, during a run, when a read finds no part while a part's `CS` pad routes nowhere. Early in a run it may be another device's read before the firmware has set this one up; if the end-of-run message above follows, it was the miswiring |
| `an SPI read on bus B CS S went to no part: NAME is the device on GPIO N, which the firmware drives as a chip select, and the pad was low, where high selects it` | logged once: a read on the bus of a part on a GPIO chip select while that pad was not at its selecting level. Another device's read, or the claim's `cs_active_high` is the wrong way round |
| `SPI bus B: GPIO N (NAME) and GPIO M (OTHER) are both at their selecting level ...` | two parts on GPIO chip selects of one bus were both selected as a transfer was clocked; neither is asked |
| `SPI bus B CS S: NAME on GPIO N and OTHER both select it ...` | two parts' `CS` pads are routed to one chip select; only the one on the lower pad is asked |
| `NAME: SPI: chip select GPIO N is on bus B but GPIO M is a bus C line ...` | the part's clock or data pad is on another SPI bus than its chip select |
| `NAME is the SPI device on chip select GPIO N, bus B, but GPIO M never carried a line of bus B ...` | logged at the end of a command-line run: the board's clock or data pad is not one the firmware put that bus's lines on, which the message lists |
| `handling a frame from the emulator: ...` or `handling 'input': ...` | the part crashed (a trap, a bad buffer, bytes that do not decode); it is stopped and gets no further calls |
| `a call ran longer than 1s and was stopped` | the part is stuck in a loop; the command-line emulator stops it as it does a crash |
| `could not fetch ./name.wasm: HTTP 404` | no file at the part's URL. A package named `my-part` builds `my_part.wasm` |
| `no part attached within 5 s` | the part never finished loading; the `part:` line before it, or the browser console, says why |
| *(none: the part attaches, then nothing happens)* | a pin name the part does not have (names are case-sensitive; in the command-line emulator its attach line says `drives no pin`), or an interaction it does not have |
