//! A push button, written as a virtual part.
//!
//! A part is a WebAssembly module that models a component wired to the chip's
//! pins. This one is a momentary button between a GPIO and ground: the pin
//! rests high on the chip's pull-up, and a press pulls it low. It is the button
//! the demo page runs.
//!
//! `PushButton` is the device and knows nothing about WebAssembly. The
//! `export_part!` line at the end makes it a part: the `esp-part` crate
//! exports every function of `proto/PART_ABI.md` and moves the bytes between
//! the shared buffer and the device.

use esp_part::prelude::*;

/// The button's own name for its signal pin. The board says which chip pin it
/// is wired to, as `{ pins: { A: 9 } }`.
const SIGNAL: &str = "A";

#[derive(Default)]
struct PushButton {
    /// The chip pin `A` is wired to. `None` until the board says, or when it
    /// left the pin unconnected: an unwired button drives nothing.
    pin: Option<u32>,
    /// True while the contact is closed. Released matches an undriven pin,
    /// which reads high through the pull-up, so there is nothing to announce
    /// at startup.
    down: bool,
    /// What the emulator was last told. `poll` speaks only when this differs
    /// from `down`, so a press is one message, not one per poll.
    reported: bool,
}

impl Device for PushButton {
    const NAME: &'static str = "pushbutton";

    /// Where the board wired the part.
    fn configure(&mut self, config: &PartConfig) {
        self.pin = config.pins.get(SIGNAL).copied();
    }

    /// Which pins this part drives. Claiming a pin is what lets a part drive
    /// it, and what stops a second part from driving it too.
    fn claims(&self) -> Vec<Claim> {
        self.pin
            .map(|pin| drives(1 << pin))
            .into_iter()
            .collect()
    }

    /// Somebody used the part. The names are the part's own; the demo page
    /// sends them from its Press and Release buttons.
    fn on_input(&mut self, name: &str) {
        match name {
            "press" => self.down = true,
            "release" => self.down = false,
            _ => {} // Not an interaction this part has.
        }
    }

    /// What to tell the emulator now, if anything.
    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        let pin = self.pin?;
        if self.down == self.reported {
            return None;
        }
        self.reported = self.down;
        // Pressed pulls the line low; released lets the pull-up take it high.
        // Stamped `now_ns`: the press has already happened.
        Some(drive(pin, !self.down, now_ns))
    }

    fn state(&self) -> Option<State> {
        Some(State::new().set("pressed", self.down))
    }
}

esp_part::export_part!(PushButton, buffer = 256);

// The device runs on the host too, so `cargo test` checks it without an
// emulator.
#[cfg(test)]
mod tests {
    use super::*;
    use esp_part::Payload;

    fn wired_to(pin: u32) -> PushButton {
        let mut button = PushButton::default();
        button.configure(&PartConfig { pins: [(SIGNAL.to_string(), pin)].into(), ..Default::default() });
        button
    }

    fn drive(msg: Option<EmulatorMessage>) -> (u32, bool) {
        match msg.and_then(|m| m.payload) {
            Some(Payload::GpioDrive(d)) => (d.pin, d.level),
            other => panic!("expected a GpioDrive, got {other:?}"),
        }
    }

    #[test]
    fn a_press_pulls_the_pin_low_and_a_release_lets_it_go() {
        let mut button = wired_to(9);
        button.on_input("press");
        assert_eq!(drive(button.poll(1_000)), (9, false));
        button.on_input("release");
        assert_eq!(drive(button.poll(2_000)), (9, true));
    }

    #[test]
    fn it_speaks_once_per_change() {
        let mut button = wired_to(9);
        assert!(button.poll(0).is_none(), "nothing has happened yet");
        button.on_input("press");
        assert!(button.poll(1).is_some());
        assert!(button.poll(2).is_none(), "still pressed, nothing new");
    }

    #[test]
    fn it_claims_the_pin_it_is_wired_to_and_no_other() {
        let claims = wired_to(4).claims();
        assert_eq!(claims, [drives(1 << 4)]);
        assert!(PushButton::default().claims().is_empty(), "an unwired button claims nothing");
    }

    #[test]
    fn an_interaction_it_does_not_have_changes_nothing() {
        let mut button = wired_to(9);
        button.on_input("wiggle");
        assert!(button.poll(0).is_none());
    }

    #[test]
    fn it_shows_whether_it_is_pressed() {
        let mut button = wired_to(9);
        assert_eq!(button.state().unwrap().get("pressed"), Some("false"));
        button.on_input("press");
        assert_eq!(button.state().unwrap().get("pressed"), Some("true"));
    }
}
