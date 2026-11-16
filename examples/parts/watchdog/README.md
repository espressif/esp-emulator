# Watchdog: an example part on the chip's EN pin

A virtual windowed watchdog supervisor, written in Rust. The firmware kicks
it from its control loop; when the kicks stop, or come too fast, it pulls the
chip's EN (CHIP_PU) pin low and the chip is held in reset, then boots as from
power-on. It is the part the
[watchdog board](../../boards/watchdog/README.md) runs. To start a part of
your own, copy the simpler [push button](../pushbutton/README.md) and follow
its "Make your own part" steps; this one shows what a part on EN adds: the
`en` claim, `drive_en` and `on_en`.

## What it models

A supervisor with a watchdog input, `WDI`, and an open-drain reset output,
`RESET`, of the kind found beside a microcontroller in safety-related
designs. No one device: the times are round numbers such parts have.

| Time | Value | Meaning |
|---|---|---|
| closed window | 50 ms | a kick sooner than this after the last one is a fault |
| window | 1 s | no kick for this long is a fault |
| startup | 3 s | after a reset the first kick may take this long, and is never too early |
| reset time | 200 ms | how long `RESET` is held low |

Either edge of `WDI` is a kick. On a fault the part pulls EN low
(`drive_en(false, now)`) and, from its next `poll`, queues the release 200 ms
later (`drive_en(true, until)`): the release stays queued while the chip is
held, so the pulse is 200 ms on the chip's clock. While EN is low, by this
part or by anything else on the line, its count stands still, as a
supervisor's does while its own reset line is low; it starts again, with the
startup time, when the chip comes back.

Wire it in a board file with `RESET` on EN, which has no GPIO number:

```json
{ "parts": { "wdt": { "module": "watchdog.wasm", "pins": { "WDI": 4, "RESET": "EN" } } } }
```

Left off EN, it still watches `WDI` and counts faults, and resets nothing.

## What it does not model

- Supply supervision: it never resets for an undervoltage.
- The tolerances of a real part's timings, and a watchdog-disable pin.

## Interactions and state

| Name | What it does |
|---|---|
| `reset` | The manual reset input: one reset pulse now. |

Its state is `resets` (how many pulses so far), `cause` (`late`, `early`,
`manual`, or `none`) and `resetting` (whether its pulse is in progress).

## Build it

You need Rust (<https://rustup.rs>) and its WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

Then, in this directory:

```sh
cargo build --release --target wasm32-unknown-unknown
```

The part is `target/wasm32-unknown-unknown/release/watchdog.wasm`.
`cargo test` runs the part's own tests on your computer, without the emulator.
