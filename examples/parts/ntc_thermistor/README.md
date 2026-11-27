# NTC thermistor: an example analog part

A virtual thermistor module, written in Rust: the divider many temperature
boards carry, read by the firmware with the stock `adc_oneshot` driver. It is
the second part of the [analog board](../../boards/analog/README.md); the
[potentiometer](../potentiometer/README.md) beside it is the simpler one.

## What it models

A 10 kΩ fixed resistor from the 3.3 V rail to the output and a 10 kΩ (at
25 °C) NTC thermistor, B 3950, from the output to ground. The output is on an
ADC pad:

```text
R(T) = 10 kΩ · exp(3950 · (1/T − 1/298.15 K))
V    = 3300 mV · R(T) / (10 kΩ + R(T))
```

1650 mV at 25 °C, 871 mV at 50 °C, 2543 mV at 0 °C: the output falls as the
thermistor warms. The part claims the pad and holds the output's voltage
there.

Set to a new temperature, the thermistor warms or cools toward it as a small
bead does in a stirred liquid: each second closes 63 % of the gap, a
one-second thermal time constant. The part sends each change as one
`AnalogWave`, the output a hundred times a second from the moment it was set
until it settles, so the firmware's readings ramp rather than jump. A
temperature set during a change starts from where the change had got to.
After a chip reset, which drops the rest of a change in progress, the part
sends that rest again. Nothing else moves the output.

## What it does not model

- Self-heating, and a time constant that changes with what the thermistor
  sits in: it is one second, in still air it would be tens of seconds.
- Tolerance of the resistors and of B.

## Interactions

| Name | What it does |
|---|---|
| `warmer`, `cooler` | 1 °C up or down from the temperature it was set to, reached as any change is. |
| `set:temperature=<°C>` | The temperature it warms or cools toward. The value is a plain decimal, such as `37.5`. |

It starts at 25 °C and stays between −40 °C and 125 °C.

## Build it

As the [potentiometer](../potentiometer/README.md#build-it); the part is
`target/wasm32-unknown-unknown/release/ntc_thermistor.wasm`.

## Put it on a board

```json
{ "parts": { "ntc": { "module": "ntc_thermistor.wasm", "pins": { "OUT": 3 } } } }
```

The pin must be an ADC pad of the chip, as for the potentiometer.

## What is in it

| File | What it does |
|---|---|
| `Cargo.toml` | A `cdylib` crate on [`esp-part`](../../../esp-part/README.md). |
| `src/lib.rs` | The module (`Thermistor`), as an `esp_part::Device` whose `poll` holds the divider's output on its pad at start-up and sends each change of temperature as one `AnalogWave` (`esp_part::wave`), and the `export_part!` line with a 4096-byte buffer for the longest change. |
