# esp-part: write a virtual part

A part is a WebAssembly module that models a component wired to the chip's
pins — a button, a sensor, an LED — which the emulator or a page loads and
talks to through the functions of [`proto/PART_ABI.md`](../proto/PART_ABI.md)
and the messages of [`proto/esp_cosim.proto`](../proto/esp_cosim.proto).

This crate is that ABI, done once. A part is a type that implements `Device`
and one line of `export_part!`:

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

| `Device` | What it is | Who needs it |
|---|---|---|
| `NAME` | The part's name, as the emulator and a page see it. | every part |
| `configure(config)` | Where the board wired the part, by its own pin names (`config.pins.get("A")`), and what it strapped (`config.addr`). | every part |
| `claims()` | The pins it drives (`drives(pins)`), the I2C address or the SPI chip select it is, and the pins it wants every transition of (`watches(pins)`; any other pin arrives once a tick). The emulator refuses a claim another part holds. | every part |
| `on_pins(levels, now)` | Every pin's level, bit N for GPIO N, after the handshake, after a chip reset and whenever a pin moves. | a part that reads a pin |
| `poll(now)` | What to tell the emulator now: a pin to drive (`drive(pin, level, now)`), or the millivolts on an analog pad it claimed (`analog(pin)`, then `hold(pin, mv, now)`, held until the next). One message a poll. Asked after every frame and every interaction. | a part that drives a pin or a pad |
| `ON_BUS`, `on_bus(request)` | A transaction on the bus it claimed; the reply quotes the request's `correlation_id`. `ON_BUS = true` opts in, which keeps the decoder out of a part that has no bus. | an I2C or SPI device |
| `ON_MESSAGE`, `on_uart(bytes)` | What the chip transmitted on the serial line it claimed (`serial(tx, rx)`, by the device's own TX and RX pins), in pieces. It answers from `poll` with `transmit(bytes, now)`; nothing waits on it. `ON_MESSAGE = true` opts in, which keeps the message decoder out of a part that is sent no messages. | a serial device |
| `ON_MESSAGE`, `on_pwm(state)` | The waveform a PWM block puts on a pin it reads as PWM (`reads_pwm(pins)`): `period_ns` and `high_ns`, when they change. Period 0 is no waveform. `ON_MESSAGE = true` opts in, as for a serial device. | a servo, a buzzer, a dimmed LED, a motor driver |
| `en()`, `drive_en(level, now)`, `on_en(level, at)` | The chip's EN pin, for a part the board wired to it (`config.en_pins` names the pin): the claim, the drive from `poll` (`false` holds the chip in reset while emulated time goes on, `true` lets it boot as from power-on; both ends of a pulse may be said ahead), and, with `ON_MESSAGE = true`, being told when EN moves, whoever moved it. | a watchdog, a supervisor, a reset button |
| `on_input(name)` | Somebody interacted: `"press"`, `"warmer"`, `"set:temperature=30.5"` (`set_value` parses the last). | a part people act on |
| `state()` | What it shows, as named values. | a part people look at |

`export_part!(Device)` exports every `part_*` function over a 512-byte
buffer; `export_part!(Device, buffer = 1024)` sizes the buffer, which holds
every message either side passes.

## Build a part

```sh
rustup target add wasm32-unknown-unknown
cargo build --release --target wasm32-unknown-unknown
```

in a `cdylib` crate that depends on this one. [`examples/parts/pushbutton`](../examples/parts/pushbutton)
is one to copy, with [`sht31`](../examples/parts/sht31) (I2C),
[`tmp123`](../examples/parts/tmp123) (SPI),
[`eeprom93c46`](../examples/parts/eeprom93c46) (SPI on a GPIO chip select,
driving its busy signal back), [`mhz19b`](../examples/parts/mhz19b) (a
serial device), [`ky040`](../examples/parts/ky040) (a rotary encoder's
quadrature edges, counted by the chip's PCNT) and [`sg90`](../examples/parts/sg90)
(a servo read as PWM) beside it. The device runs on the host too, so `cargo test` checks it without an emulator.

The contract's types are generated from `../proto/esp_cosim.proto` when this
crate builds, by protox, so no `protoc` is needed. A copy of this crate kept
outside the repository sets `ESP_COSIM_PROTO_DIR` to the folder holding its
copy of the `.proto`.
