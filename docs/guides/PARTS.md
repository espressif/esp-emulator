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

**What works today:** parts that read and drive GPIO pins, I2C devices and SPI
devices. Other signals, such as UART, are planned.

| Example | What it is | Where |
|---|---|---|
| Push button | a button on a GPIO | [part](../../examples/parts/pushbutton), [board](../../examples/boards/button/README.md) |
| SHT31 | a temperature and humidity sensor on I2C | [part](../../examples/parts/sht31), [board](../../examples/boards/sensor/README.md) |
| TMP123 | a temperature sensor on SPI | [part](../../examples/parts/tmp123), [board](../../examples/boards/thermometer/README.md) |
| 93C46 EEPROM | an SPI EEPROM whose chip select the firmware drives as a GPIO | [part](../../examples/parts/eeprom93c46), [board](../../examples/boards/eeprom/README.md) |

More parts will come from **esp-virtual-parts**, a separate project that will
publish one `.wasm` file per component, plus an `index.json` listing each
part's pin names and the interactions it understands.

## Try it in five minutes

Download two files from the
[latest release](https://github.com/espressif/esp-emulator/releases/latest):
the emulator for your computer, `esp-emu-<version>-<platform>.tar.gz`, and the
examples kit, `esp-emu-<version>-examples.tar.gz`. Unpack both. The kit has a
`button/`, a `sensor/` and a `thermometer/` folder. Each holds a board file,
the part and ESP32-C3 firmware, ready to run.

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
same way, with `warmer` and `cooler` instead of `press` and `release`.

### In the browser

The same button and sensor run in a web page, with nothing to install:

- <https://espressif.github.io/esp-emulator/button-demo.html>
- <https://espressif.github.io/esp-emulator/sensor-demo.html>

Everything runs in your browser. Nothing is uploaded. The log shows the part
attaching, then the firmware's output. Click **Press** and the firmware prints
`BUTTON: pressed`. On the sensor page, **Warmer**, **Cooler**, **Wetter** and
**Drier** change what the part measures, and the next reading follows.

To run the pages locally, download `esp-emu-<version>-wasm.tar.gz` from the
release, unpack it, and serve its `www/` folder with the server that comes in
it:

```sh
cd esp-emu-<version>-wasm/www
python3 serve.py 8080
# open http://localhost:8080/button-demo.html
```

`www/` holds the demo firmware and parts as `_button_fw.bin`,
`pushbutton.wasm`, `_sensor_fw.bin` and `sht31.wasm`. Replace them to run your
own; with either file of a demo missing, its page asks for both. `serve.py` tells the browser not to cache, so a part you rebuild and copy
in is the one the next reload runs. The demo pages only know the push button
and the SHT31. Any other part runs in
[a page of your own](#use-parts-in-your-own-page).

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
  button calls its signal pin `A`, so `{ "A": 9 }` puts it on GPIO 9.

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

**At exit** the emulator reports how long the parts took, any part that
failed, and any wiring the firmware never used: an I2C part whose pads never
got a bus, or an SPI part whose chip-select pad never got a chip select, with
the pads the firmware did use. It also reports the worst delay of any
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
| `Register`, `RegisterAck` | the part attaching, and the emulator accepting it |
| `ChipReset` | the chip was reset |
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

All four examples show what they are set to through `state`, so
`part NAME state` and a web page can read it.

### Time inside a part

A part never sleeps or blocks. It is told the emulated time, `now_ns`, and
says what should happen and when. The usual way to act later, such as a
sensor releasing its pin two seconds after a trigger, is to keep the deadline
in the part's own state and act when a later `poll` sees the clock pass it.
That lands within one heartbeat, a millisecond.
[`PART_ABI.md`](../../proto/PART_ABI.md#two-ways-to-act-at-a-time-and-when-to-use-each)
shows the code and explains the second, finer-grained way.

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
| `start(onOutput, onError)` | Runs the firmware, paced to the wall clock. `onOutput` gets the UART output, one or more whole lines per call. `onError` is called once if a batch throws, or if a part has not answered a bus transaction after 5 s, and the loop stops; in the second case every `Worker` the constructor was given is terminated first, so a spinning part does not go on taking a core. A restart the firmware asks for is performed; the parts stay attached. |
| `stop()` | Stops the loop and flushes the last unfinished line to `onOutput`. |
| `drain()` | Delivers every waiting frame to the parts. Only for a page that runs its own loop with `emu.run_batch` instead of `start`; such a page also decides how long to wait while `emu.cosim_awaiting_device()` says a part is answering. |

**`part-worker.js`** messages:

| Direction | Message | Meaning |
|---|---|---|
| page → worker | `{ type: "start", port, module, config }` | Load the part. `port` is the worker's end of the part's `MessageChannel` (transfer it); `module` is the `.wasm` URL or its bytes as an `ArrayBuffer`; `config` is `{ pins: { NAME: gpio }, addr }`, with `addr` only for an I2C part that takes one. An SPI part is wired by its pins alone. |
| page → worker | `{ type: "input", kind }` | Somebody interacted with the part: `kind` is the interaction's name, such as `"press"`. |
| page → worker | `{ type: "state" }` | Somebody is looking at the part: ask what it shows. |
| worker → page | `{ type: "state", values }` | What the part shows, as `{ name: "value" }` in the part's own words, or `null` for a part that shows nothing. |
| worker → page | `{ type: "loaded", name }` | The module loaded; `name` is the name the part gives itself. |
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

## When something goes wrong

| Message | Meaning |
|---|---|
| `part is missing part_configure, ...` | the file is not a part, or was built for a different part ABI |
| `part speaks ABI N; this host speaks M` | the part and this emulator release were built for different part ABIs; get matching versions |
| `refused: protocol vN != emulator vM ...` | the part and the emulator speak different contract versions |
| `refused: GPIO N is already driven by another model` | two parts are wired to the same pin |
| `refused: GPIO N is beyond this chip's M pins` | the part is wired to a pin this chip does not have |
| `pin A is GPIO N, beyond this chip's M pins` | the board file wires a pin this chip does not have (command-line emulator) |
| `part claims an I2C address but exports no part_on_bus` | an I2C part built without its bus handler |
| `part claims an SPI chip select but exports no part_on_bus` | the same, for an SPI part |
| `refused: an SPI claim names no chip select (cs_pin) ...` | the SPI part is wired without its `CS`; pin names are case-sensitive |
| `refused: SPI chip select GPIO N is already claimed ...` | two parts are wired to one chip-select pad |
| `a part did not answer within 5 s: an SPI read on bus B CS S, from NAME` | the part failed while answering a read, as for I2C below, with `BUS_OP_SPI` |
| `A device model did not answer: an SPI read on bus B CS S, from NAME, which could not be asked ...` | the same, from the command-line emulator, which stops at once |
| `NAME answered N bytes to a M-byte read on SPI bus B CS S; ...` | the part's answer is not the length the firmware reads: the rest reads 0xFF, or the extra is dropped. Logged at the first and then at each doubling of the count |
| `NAME: the board gives it addr N (0xNN), an I2C address, but it claims none ...` | `addr` on a part that is no I2C device; it is ignored |
| `refused: I2C 0xNN has no scl_pin ...` (or `sda_pin`) | the part is wired without `SCL` or `SDA`; the names are upper case |
| `refused: I2C 0xNN on GPIO A/B is already claimed ...` | two parts are wired as the same device on the same bus |
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
