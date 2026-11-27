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
| `poll(now)` | What to tell the emulator now: a pin to drive (`drive(pin, level, now)`), or the millivolts on an analog pad it claimed (`analog(pin)`, then `hold(pin, mv, now)`, held until the next, or a run of them one period apart, `wave(pin, period_ns, mvs, now)`). Asked after every frame and every interaction, and again with the same `now` until it returns `None` (at most 256 times), so it hands over all it owes at once. | a part that drives a pin or a pad |
| `play(pin, first_level, widths, at)` | A whole waveform on a pin it drives, as one message from `poll`: `first_level` from `at`, the pin toggling after each width in nanoseconds and holding the last level. Late, it starts at once with every width kept. At most 512 widths (a buffer of 2,581 bytes holds the longest). | a DHT22, a 1-Wire device, an ultrasonic sensor |
| `ON_BUS`, `on_bus(request)` | A transaction on the bus it claimed; the reply quotes the request's `correlation_id`. `ON_BUS = true` opts in, which keeps the decoder out of a part that has no bus. | an I2C or SPI device |
| `ON_MESSAGE`, `on_uart(bytes)` | What the chip transmitted on the serial line it claimed (`serial(tx, rx)`, by the device's own TX and RX pins), in pieces. It answers from `poll` with `transmit(bytes, now)`; nothing waits on it. `ON_MESSAGE = true` opts in, which keeps the message decoder out of a part that is sent no messages. | a serial device |
| `ON_MESSAGE`, `on_pwm(state)` | The waveform a PWM block puts on a pin it reads as PWM (`reads_pwm(pins)`): `period_ns` and `high_ns`, when they change. Period 0 is no waveform. `ON_MESSAGE = true` opts in, as for a serial device. | a servo, a buzzer, a dimmed LED, a motor driver |
| `ON_MESSAGE`, `reads_edges(pins)`, `on_edges(pin, first_level, widths, at)` | The edges of a pin it reads in runs: a first level at `at` and the widths after it, in nanoseconds, up to 512 a run, at most a heartbeat after they happened; `on_pins` follows each run with the pin's last level. A run is up to 2,581 bytes, so the buffer must hold that (`export_part!(Device, buffer = 4096)`). | an LED strip, a reader of an IR LED or an RF transmitter |
| `en()`, `drive_en(level, now)`, `on_en(level, at)` | The chip's EN pin, for a part the board wired to it (`config.en_pins` names the pin): the claim, the drive from `poll` (`false` holds the chip in reset while emulated time goes on, `true` lets it boot as from power-on; both ends of a pulse may be said ahead), and, with `ON_MESSAGE = true`, being told when EN moves, whoever moved it. | a watchdog, a supervisor, a reset button |
| `ON_MESSAGE`, `on_i2s_format(format, at)`, `on_i2s_out(pcm, at)` | The layout of each direction of the I2S data pads it claimed (`i2s(din, dout)`, by the device's own data pins), when the firmware starts it and whenever it changes, and the samples the chip clocked out, at most 4096 bytes at a time. A microphone answers from `poll` with `samples(pcm, now)`, paced as `proto/PART_ABI.md` says; `audio::Frame` reads and writes one slot of a frame. `ON_MESSAGE = true` opts in, as for a serial device. | an audio device |
| `ON_MESSAGE`, `on_dvp_format(format, at)`, `on_csi_format(format, at)` | What the camera receiver it is the sensor for looks like (`dvp(pclk, vsync, data, href, xclk)` by the sensor's own pins, or `csi()`): whether it captures, its session, and the XCLK or the lanes, when they change. Its frames go out from `poll` as `camera::Stream` paces them and cuts them to the buffer (`camera::piece_for`, with the `PART_BUFFER` that `export_part!` defines); `camera::bars` is a sensor's colour-bar pattern in any raw layout. `camera::Sensor` does all of that for a device that decodes its mode and rate from its registers: the pacing, `set:fps=`, the bars and the common state. `ON_MESSAGE = true` opts in, as for a serial device. | a camera sensor |
| `video_format()`, `video_in(picture)` | What it asks to be shown (`VideoFormat`: size, fourcc, rate), asked before each picture, and the picture, whole: RGB888 of that size, or a JPEG for a JPEG mode. `video_in` returns `Err` with why it did not keep one, which the host reports. `camera::Sensor` answers both for a camera: it asks for nothing larger than the buffer, makes RGB888 into the mode's layout (`camera::convert`) and refuses a picture made for a mode the firmware has since left. Exported by `export_part!(Device, buffer = N, sees)`. | a camera that streams what a webcam or a folder of frames shows it |
| `on_input(name)` | Somebody interacted: `"press"`, `"warmer"`, `"set:temperature=30.5"` (`set_value` parses the last). | a part people act on |
| `state()` | What it shows, as named values. | a part people look at |
| `render()` | What it draws, as bytes in a layout the part states (a screen's size and pixel format), or `None` when nothing is new since the last call. Must fit the buffer. | a lamp, a display |
| `audio_out(out)`, `audio_in(pcm)`, `audio_rate(receive)` | The sound it makes, for somebody to hear, written into `out` (whole samples; it keeps the rest), and a sound somebody makes at it: mono 16-bit little-endian samples at the rate `audio_rate` gives for each direction, 0 while it does not know. Exported only with the `sounds` and `hears` markers. | an amplifier, a microphone, a codec |

`export_part!(Device)` exports every `part_*` function over a 512-byte
buffer; `export_part!(Device, buffer = 1024)` sizes the buffer, which holds
every message either side passes: an audio device that listens needs 4114
bytes. A part that makes a sound or hears one adds `sounds`, `hears` or both
after it, `export_part!(Amp, buffer = 8192, sounds)`, which exports
`part_audio_out`, `part_audio_in` and `part_audio_rate`: a host reads those
exports as what the part does with sound.

## Build a part

```sh
rustup target add wasm32-unknown-unknown
cargo build --release --target wasm32-unknown-unknown
```

in a `cdylib` crate that depends on this one. [`examples/parts/pushbutton`](../examples/parts/pushbutton)
is one to copy, with [`sht31`](../examples/parts/sht31) (I2C),
[`tmp123`](../examples/parts/tmp123) (SPI),
[`eeprom93c46`](../examples/parts/eeprom93c46) (SPI on a GPIO chip select,
driving its busy signal back), [`potentiometer`](../examples/parts/potentiometer)
(a voltage held on an ADC pad), [`watchdog`](../examples/parts/watchdog) (the
chip's EN pin), [`dht22`](../examples/parts/dht22) (a whole waveform with
`play`), [`ws2812`](../examples/parts/ws2812) (a pin read as runs of edges
with `on_edges`), [`mhz19b`](../examples/parts/mhz19b) (a
serial device), [`ky040`](../examples/parts/ky040) (a rotary encoder's
quadrature edges, counted by the chip's PCNT), [`sg90`](../examples/parts/sg90)
(a servo read as PWM), [`inmp441`](../examples/parts/inmp441) and
[`max98357a`](../examples/parts/max98357a) (a microphone and an amplifier on
I2S), [`vs1838b`](../examples/parts/vs1838b) (an IR receiver whose NEC
frames the chip's RMT times, handed over a frame at a time),
[`ssd1306`](../examples/parts/ssd1306) (an OLED screen whose picture `render`
hands out), and [`ov2640`](../examples/parts/ov2640) and
[`ov5647`](../examples/parts/ov5647) (camera sensors on DVP and on MIPI CSI,
whose frames, larger than their buffer, go out in pieces at their own rate)
beside it. The device runs on the host too, so `cargo test` checks
it without an emulator.

The contract's types are generated from `../proto/esp_cosim.proto` when this
crate builds, by protox, so no `protoc` is needed. A copy of this crate kept
outside the repository sets `ESP_COSIM_PROTO_DIR` to the folder holding its
copy of the `.proto`.
