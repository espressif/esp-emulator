//! A windowed watchdog supervisor, written as a virtual part.
//!
//! The firmware toggles the supervisor's `WDI` input from its control loop.
//! A toggle too soon after the last one, or none before the window closes,
//! and the supervisor pulls its `RESET` output low for its reset time. On a
//! board that output goes to the chip's EN (CHIP_PU) pin, so the chip is held
//! in reset and then boots as from power-on.
//!
//! No one device: the times are round numbers of the kind such parts have.
//! `RESET` is open-drain like the real ones, so a reset button or a second
//! supervisor may share EN with it.
//!
//! A board wires it as `{ "pins": { "WDI": 4, "RESET": "EN" } }`.

use esp_part::prelude::*;

/// The input the firmware toggles: either edge is a kick.
const WDI: &str = "WDI";
/// The open-drain output, wired to the chip's EN.
const RESET: &str = "RESET";

const MS: u64 = 1_000_000;
/// A kick sooner than this after the last one is a fault: a loop spinning
/// through its kick without doing its work.
const WINDOW_CLOSED_NS: u64 = 50 * MS;
/// No kick for this long is a fault: a loop that hung.
const WINDOW_NS: u64 = 1_000 * MS;
/// After a reset the first kick may take this long, as boot does. An early
/// kick is not a fault before the first one either.
const STARTUP_NS: u64 = 3_000 * MS;
/// How long `RESET` is held low.
const RESET_NS: u64 = 200 * MS;

#[derive(Default, Clone, Copy, PartialEq, Debug)]
enum Phase {
    /// Counting from `since`; `kicked` once the firmware has kicked at all.
    #[default]
    Watching,
    /// A fault was seen: `RESET` goes low at the next poll.
    Tripped,
    /// `RESET` is low, from `since`, and the release is not said yet.
    Low,
    /// The release is queued; waiting for the chip to come back.
    Releasing,
}

#[derive(Default)]
struct Watchdog {
    wdi: Option<u32>,
    /// Whether the board wired `RESET` to EN. Unwired, it watches and counts
    /// and resets nothing.
    on_en: bool,
    phase: Phase,
    /// When the current window opened, or `RESET` went low.
    since: u64,
    kicked: bool,
    /// `WDI` as last seen; `None` before the first report.
    level: Option<bool>,
    /// The time of the last poll, to notice the chip's clock starting over.
    last_now: u64,
    /// EN is low, by this part or another on the line: the count stands
    /// still, as a supervisor's does while its own reset line is low.
    line_low: bool,
    resets: u32,
    cause: &'static str,
}

impl Watchdog {
    fn trip(&mut self, cause: &'static str) {
        if self.phase == Phase::Watching {
            self.phase = Phase::Tripped;
            self.cause = cause;
        }
    }

    /// Start over, as after the supervisor's own reset.
    fn rearm(&mut self, now_ns: u64) {
        self.phase = Phase::Watching;
        self.since = now_ns;
        self.kicked = false;
    }

    /// Note the time of a call. True when the chip's clock started over: it
    /// left reset, its own or any other, and the count starts over with it.
    fn clock_restarted(&mut self, now_ns: u64) -> bool {
        let restarted = now_ns < self.last_now;
        self.last_now = now_ns;
        if restarted {
            self.rearm(now_ns);
        }
        restarted
    }
}

impl Device for Watchdog {
    const NAME: &'static str = "watchdog";
    /// To be told when EN moves (`on_en`).
    const ON_MESSAGE: bool = true;

    fn configure(&mut self, config: &PartConfig) {
        self.wdi = config.pins.get(WDI).copied();
        self.on_en = config.en_pins.iter().any(|p| p == RESET);
    }

    fn claims(&self) -> Vec<Claim> {
        // Every edge of WDI at its own time: the window is measured from it.
        let wdi = self.wdi.map(|pin| watches(1 << pin));
        wdi.into_iter().chain(self.on_en.then(en)).collect()
    }

    fn on_pins(&mut self, levels: u64, now_ns: u64) {
        let Some(pin) = self.wdi else { return };
        let level = levels >> pin & 1 == 1;
        let was = self.level.replace(level);
        // A reset lets WDI go: the level it reports is not an edge, and the
        // time before it is no measure of the time since.
        let reset = self.clock_restarted(now_ns);
        // Nor is the first report; and a chip held in reset lets WDI go too.
        if reset || was.is_none() || was == Some(level) || self.phase != Phase::Watching {
            return;
        }
        if self.kicked && now_ns.saturating_sub(self.since) < WINDOW_CLOSED_NS {
            self.trip("early");
        } else {
            self.since = now_ns;
            self.kicked = true;
        }
    }

    fn on_en(&mut self, level: bool, at_ns: u64) {
        if !self.on_en {
            return;
        }
        self.line_low = !level;
        // Let go by the last holder, which may not be this part: the count
        // starts from here, and again from the chip's new clock.
        if level && self.phase != Phase::Tripped {
            self.rearm(at_ns);
        }
    }

    fn on_input(&mut self, name: &str) {
        // The manual reset input such devices have.
        if name == "reset" {
            self.trip("manual");
        }
    }

    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        self.clock_restarted(now_ns);
        let limit = if self.kicked { WINDOW_NS } else { STARTUP_NS };
        if self.phase == Phase::Watching && !self.line_low && now_ns.saturating_sub(self.since) >= limit {
            self.trip("late");
        }
        match self.phase {
            Phase::Watching => None,
            Phase::Tripped => {
                self.resets += 1;
                self.phase = Phase::Low;
                self.since = now_ns;
                self.on_en.then(|| drive_en(false, now_ns))
            }
            // Said ahead of its time: it stays queued while the chip is held.
            Phase::Low => {
                self.phase = Phase::Releasing;
                self.on_en.then(|| drive_en(true, self.since + RESET_NS))
            }
            Phase::Releasing => {
                // Wired to EN the clock starting over ends this; unwired, or
                // with another part still holding EN, the time does.
                if now_ns >= self.since + RESET_NS && !self.line_low {
                    self.rearm(now_ns);
                }
                None
            }
        }
    }

    fn state(&self) -> Option<State> {
        Some(
            State::new()
                .set("resets", self.resets)
                .set("cause", if self.resets == 0 { "none" } else { self.cause })
                .set("resetting", matches!(self.phase, Phase::Low | Phase::Releasing)),
        )
    }
}

esp_part::export_part!(Watchdog, buffer = 256);

#[cfg(test)]
mod tests {
    use super::*;
    use esp_part::{EnDrive, Payload};

    const PIN: u32 = 4;

    fn on_board() -> Watchdog {
        let mut w = Watchdog::default();
        w.configure(&PartConfig {
            pins: [(WDI.to_string(), PIN)].into(),
            en_pins: vec![RESET.to_string()],
            ..Default::default()
        });
        w.on_pins(u64::MAX, 0);
        w
    }

    /// Poll every millisecond from `from` up to `to`, collecting what it
    /// says about EN as `(level, at_ns)`.
    fn run(w: &mut Watchdog, from: u64, to: u64) -> Vec<(bool, u64)> {
        (from..to)
            .step_by(MS as usize)
            .filter_map(|t| w.poll(t))
            .map(|m| match m.payload {
                Some(Payload::EnDrive(EnDrive { level })) => (level, m.sim_time_ns),
                other => panic!("expected an EnDrive, got {other:?}"),
            })
            .collect()
    }

    /// Toggle WDI at `at_ns`.
    fn kick(w: &mut Watchdog, at_ns: u64) {
        let high = w.level.unwrap_or(true);
        w.on_pins(if high { !(1 << PIN) } else { u64::MAX }, at_ns);
    }

    #[test]
    fn it_claims_its_input_and_en() {
        assert_eq!(on_board().claims(), [watches(1 << PIN), en()]);
        assert!(Watchdog::default().claims().is_empty(), "unwired, it claims nothing");
    }

    #[test]
    fn kicks_inside_the_window_keep_reset_high() {
        let mut w = on_board();
        let mut said = Vec::new();
        for n in 0..20 {
            said.extend(run(&mut w, n * 200 * MS, (n + 1) * 200 * MS));
            kick(&mut w, (n + 1) * 200 * MS);
        }
        assert!(said.is_empty(), "{said:?}");
        assert_eq!(w.state().unwrap().get("resets"), Some("0"));
    }

    #[test]
    fn a_missed_kick_pulls_reset_for_the_reset_time() {
        let mut w = on_board();
        kick(&mut w, 100 * MS);
        let said = run(&mut w, 100 * MS, 1_300 * MS);
        let fell = 100 * MS + WINDOW_NS;
        assert_eq!(said, [(false, fell), (true, fell + RESET_NS)], "low when the window closes, the release queued");
        assert_eq!(w.state().unwrap().get("cause"), Some("late"));
        assert_eq!(w.state().unwrap().get("resetting"), Some("true"));
    }

    #[test]
    fn a_kick_too_soon_after_the_last_is_a_fault_too() {
        let mut w = on_board();
        kick(&mut w, 100 * MS);
        kick(&mut w, 100 * MS + WINDOW_CLOSED_NS - 1);
        let said = run(&mut w, 150 * MS, 160 * MS);
        assert_eq!(said, [(false, 150 * MS), (true, 150 * MS + RESET_NS)]);
        assert_eq!(w.state().unwrap().get("cause"), Some("early"));
    }

    #[test]
    fn boot_has_the_startup_time_and_its_first_kick_is_never_early() {
        let mut w = on_board();
        assert!(run(&mut w, 0, STARTUP_NS).is_empty(), "no kick yet, and no fault");
        let mut w = on_board();
        kick(&mut w, MS);
        assert!(run(&mut w, MS, 10 * MS).is_empty());
        let mut w = on_board();
        assert_eq!(run(&mut w, 0, STARTUP_NS + 2 * MS).len(), 2, "but a chip that never kicks is reset");
    }

    #[test]
    fn the_count_starts_over_when_the_chip_comes_back() {
        let mut w = on_board();
        kick(&mut w, 100 * MS);
        run(&mut w, 100 * MS, 1_200 * MS);
        // In reset the chip lets WDI go: not a kick.
        kick(&mut w, 1_150 * MS);
        // The chip's clock starts over at the release.
        assert!(run(&mut w, 0, STARTUP_NS).is_empty(), "the startup time again, counted from the new clock");
        assert_eq!(w.state().unwrap().get("resetting"), Some("false"));
        assert_eq!(w.state().unwrap().get("resets"), Some("1"));
    }

    #[test]
    fn a_reset_from_another_cause_is_not_a_kick() {
        // The firmware restarts with WDI low: the reset lets the line go, and
        // its pins are reported before the first poll on the new clock.
        let mut w = on_board();
        kick(&mut w, 100 * MS);
        run(&mut w, 100 * MS, 500 * MS);
        w.on_pins(u64::MAX, 2 * MS);
        assert!(run(&mut w, 2 * MS, STARTUP_NS).is_empty(), "the startup time, and no early fault");
        assert_eq!(w.state().unwrap().get("resets"), Some("0"));
    }

    #[test]
    fn the_count_stands_still_while_another_holds_en_low() {
        let mut w = on_board();
        kick(&mut w, 100 * MS);
        w.on_en(false, 200 * MS);
        assert!(run(&mut w, 200 * MS, 20_000 * MS).is_empty(), "a reset button held for 20 s is one reset, not many");
        w.on_en(true, 20_000 * MS);
        assert!(run(&mut w, 0, STARTUP_NS).is_empty(), "then the startup time, from the chip's new clock");
        assert_eq!(w.state().unwrap().get("resets"), Some("0"));
    }

    #[test]
    fn the_manual_input_resets_at_once() {
        let mut w = on_board();
        w.on_input("reset");
        assert_eq!(run(&mut w, 5 * MS, 8 * MS), [(false, 5 * MS), (true, 5 * MS + RESET_NS)]);
        assert_eq!(w.state().unwrap().get("cause"), Some("manual"));
    }

    #[test]
    fn off_en_it_counts_faults_and_drives_nothing() {
        let mut w = Watchdog::default();
        w.configure(&PartConfig { pins: [(WDI.to_string(), PIN)].into(), ..Default::default() });
        w.on_pins(u64::MAX, 0);
        assert!((0..2 * STARTUP_NS).step_by(MS as usize).all(|t| w.poll(t).is_none()));
        assert_eq!(w.state().unwrap().get("resets"), Some("1"), "and watches again after its reset time");
    }
}
