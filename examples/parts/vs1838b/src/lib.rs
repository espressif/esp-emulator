//! A VS1838B infrared receiver module and the remote that talks to it,
//! written as a virtual part.
//!
//! The module turns a 38 kHz infrared carrier into a level on its `OUT` pin:
//! high while no carrier arrives, low while a burst lasts. The remote sends
//! NEC frames: a 9 ms burst and a 4.5 ms space, then 32 bits least
//! significant first (the address, its inverse, the command, its inverse),
//! each a 560 us burst followed by a space of 560 us for a 0 or 1690 us for a
//! 1, and a closing 560 us burst. On `OUT` that is 68 edges over about 68 ms.
//! The part decodes nothing: the chip's RMT receiver times the edges and the
//! firmware decodes them.
//!
//! A key press plays one frame. Its edges are stamped ahead and handed over
//! together, the host asking `poll` again until it returns `None`, so each
//! lands on the emulated clock to the tick. The first is [`LEAD_IN_NS`] after
//! the poll that hands the frame over, and frames start at least
//! [`FRAME_PERIOD_NS`] apart. Presses made faster than that wait in the part,
//! so the emulator never holds more than two frames' edges.

use esp_part::prelude::*;
use std::collections::VecDeque;

/// The module's output pin, as printed on it. The board says which chip pin
/// it is wired to, as `{ pins: { OUT: 4 } }`.
const OUT: &str = "OUT";

/// The remote's address, sent with its inverse, NEC's standard form.
const ADDRESS: u8 = 0x00;

/// The remote's keys, by the input that presses each, and the command each
/// sends: digits, `star`, `hash`, four arrows and `ok`.
const KEYS: [(&str, u8); 17] = [
    ("1", 0x45),
    ("2", 0x46),
    ("3", 0x47),
    ("4", 0x44),
    ("5", 0x40),
    ("6", 0x43),
    ("7", 0x07),
    ("8", 0x15),
    ("9", 0x09),
    ("star", 0x16),
    ("0", 0x19),
    ("hash", 0x0D),
    ("up", 0x18),
    ("left", 0x08),
    ("ok", 0x1C),
    ("right", 0x5A),
    ("down", 0x52),
];

/// NEC's marks, in microseconds: the leader's burst and space, a bit's burst,
/// and the space after it for a 0 and for a 1.
const LEADER_BURST_US: u64 = 9_000;
const LEADER_SPACE_US: u64 = 4_500;
const BIT_BURST_US: u64 = 560;
const ZERO_SPACE_US: u64 = 560;
const ONE_SPACE_US: u64 = 1_690;

/// From one frame's start to the next one's: the period an NEC remote keeps
/// while a key is held.
const FRAME_PERIOD_NS: u64 = 108_000_000;

/// From the poll that hands a frame over to its first edge. A browser page
/// passes a part's answer to the emulator on a later turn of its loop, some
/// milliseconds of emulated time after the poll (the browser build's `cosim.js` keeps the
/// guest to the wall clock with waits of up to 20 ms), and a stamp already
/// passed lands at once, taking the frame's timing with it. Natively the
/// frame only starts this much later.
const LEAD_IN_NS: u64 = 50_000_000;

#[derive(Default)]
struct Vs1838b {
    /// The chip pin `OUT` is wired to; `None` where the board left it out.
    out: Option<u32>,
    /// Keys pressed and not sent yet, oldest first.
    keys: VecDeque<(&'static str, u8)>,
    /// The frame being handed over: each edge's level and time.
    edges: VecDeque<(bool, u64)>,
    /// Frames handed over that have not played out: when each ends (its
    /// closing rise), and its key.
    playing: VecDeque<(u64, &'static str, u8)>,
    /// The earliest the next frame may start.
    next_start_ns: u64,
    /// The latest poll time: a clock that goes back is a chip reset.
    last_now_ns: u64,
    /// Frames played out, and the key of the last.
    frames: u64,
    last_key: Option<(&'static str, u8)>,
}

/// One NEC frame of `command` from `start_ns`: each edge's level and time,
/// 68 of them, the last the line back high.
fn frame(command: u8, start_ns: u64) -> Vec<(bool, u64)> {
    let word = u32::from_le_bytes([ADDRESS, !ADDRESS, command, !command]);
    let mut marks = vec![(false, LEADER_BURST_US), (true, LEADER_SPACE_US)];
    for bit in 0..32 {
        let space = if word >> bit & 1 == 1 { ONE_SPACE_US } else { ZERO_SPACE_US };
        marks.extend([(false, BIT_BURST_US), (true, space)]);
    }
    marks.push((false, BIT_BURST_US));
    let mut edges = Vec::with_capacity(marks.len() + 1);
    let mut at = start_ns;
    for (level, us) in marks {
        edges.push((level, at));
        at += us * 1_000;
    }
    edges.push((true, at));
    edges
}

impl Device for Vs1838b {
    const NAME: &'static str = "vs1838b";

    fn configure(&mut self, config: &PartConfig) {
        self.out = config.pins.get(OUT).copied();
    }

    /// `OUT`, where the board wired it.
    fn claims(&self) -> Vec<Claim> {
        self.out.map(|pin| vec![drives(1 << pin)]).unwrap_or_default()
    }

    /// A key's name presses it: `0` to `9`, `star`, `hash`, `up`, `down`,
    /// `left`, `right` and `ok`. With `OUT` unwired nobody would see it.
    fn on_input(&mut self, name: &str) {
        if let Some(&key) = KEYS.iter().find(|(k, _)| *k == name).filter(|_| self.out.is_some()) {
            self.keys.push_back(key);
        }
    }

    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        let out = self.out?;
        if now_ns < self.last_now_ns {
            // The chip reset and its clock started again. The emulator dropped
            // the edges still stamped ahead and put back the last level it
            // applied, low inside a burst: a frame cut short ends with the
            // line released, and is not counted.
            let cut = !self.playing.is_empty();
            self.playing.clear();
            self.edges.clear();
            self.next_start_ns = 0;
            self.last_now_ns = now_ns;
            if cut {
                return Some(drive(out, true, now_ns));
            }
        }
        self.last_now_ns = now_ns;
        while let Some(&(_, key, command)) = self.playing.front().filter(|&&(end, ..)| end <= now_ns) {
            self.playing.pop_front();
            self.frames += 1;
            self.last_key = Some((key, command));
        }
        if self.edges.is_empty() {
            let &(key, command) = self.keys.front()?;
            let start = now_ns.div_ceil(1_000) * 1_000 + LEAD_IN_NS;
            if start < self.next_start_ns {
                return None;
            }
            self.keys.pop_front();
            self.edges.extend(frame(command, start));
            let end = self.edges.back().map_or(start, |&(_, at)| at);
            self.playing.push_back((end, key, command));
            self.next_start_ns = start + FRAME_PERIOD_NS;
        }
        let (level, at) = self.edges.pop_front()?;
        Some(drive(out, level, at))
    }

    fn state(&self) -> Option<State> {
        let (key, command) = match self.last_key {
            Some((key, command)) => (key.to_string(), command.to_string()),
            None => ("none".to_string(), "none".to_string()),
        };
        Some(State::new().set("address", ADDRESS).set("command", command).set("frames", self.frames).set("key", key))
    }
}

esp_part::export_part!(Vs1838b, buffer = 256);

#[cfg(test)]
mod tests {
    use super::*;
    use esp_part::Payload;

    const PIN: u32 = 4;

    fn wired() -> Vs1838b {
        let mut ir = Vs1838b::default();
        ir.configure(&PartConfig { pins: [(OUT.to_string(), PIN)].into(), ..Default::default() });
        ir
    }

    /// Everything the part hands over at `now_ns`, as one drained poll takes
    /// it: each edge's level and time.
    fn drain(ir: &mut Vs1838b, now_ns: u64) -> Vec<(bool, u64)> {
        std::iter::from_fn(|| ir.poll(now_ns))
            .map(|msg| match msg.payload {
                Some(Payload::GpioDrive(d)) => {
                    assert_eq!(d.pin, PIN);
                    (d.level, msg.sim_time_ns)
                }
                other => panic!("expected a GpioDrive, got {other:?}"),
            })
            .collect()
    }

    /// IDF's `ir_nec_transceiver` decode: the leader, then each bit by the
    /// space after its burst, every width within 200 us. The address and
    /// command words, least significant bit first.
    fn decode(edges: &[(bool, u64)]) -> Option<(u16, u16)> {
        let widths: Vec<(bool, u64)> = edges.windows(2).map(|w| (w[0].0, (w[1].1 - w[0].1) / 1_000)).collect();
        let near = |got: u64, want: u64| got.abs_diff(want) < 200;
        if widths.len() != 67 || widths[0].0 || !near(widths[0].1, 9_000) || !near(widths[1].1, 4_500) {
            return None;
        }
        let mut word = 0u32;
        for bit in 0..32 {
            let (burst, space) = (widths[2 + 2 * bit], widths[3 + 2 * bit]);
            if burst.0 || !near(burst.1, 560) || !space.0 {
                return None;
            }
            if near(space.1, 1_690) {
                word |= 1 << bit;
            } else if !near(space.1, 560) {
                return None;
            }
        }
        Some((word as u16, (word >> 16) as u16))
    }

    #[test]
    fn a_key_is_one_nec_frame_handed_over_in_one_poll() {
        let mut ir = wired();
        ir.on_input("ok");
        let edges = drain(&mut ir, 0);
        assert_eq!(edges.len(), 68, "leader, 32 bits and the closing burst, each a fall and a rise");
        assert!(edges.iter().enumerate().all(|(i, &(level, _))| level == (i % 2 == 1)), "falls and rises in turn");
        assert_eq!(decode(&edges), Some((0xFF00, 0xE31C)), "address 0x00 and command 0x1C, each with its inverse");
        assert!(drain(&mut ir, 0).is_empty(), "nothing left once it has been handed over");
    }

    #[test]
    fn every_key_sends_its_own_command() {
        for (name, command) in KEYS {
            let mut ir = wired();
            ir.on_input(name);
            let want = (!command as u16) << 8 | command as u16;
            assert_eq!(decode(&drain(&mut ir, 0)), Some((0xFF00, want)), "key {name}");
        }
    }

    #[test]
    fn the_frame_is_stamped_ahead_on_whole_microseconds() {
        let mut ir = wired();
        ir.on_input("1");
        let edges = drain(&mut ir, 1_234_567);
        assert_eq!(edges[0].1, 1_235_000 + LEAD_IN_NS, "the lead-in from the next whole microsecond");
        assert!(edges.iter().all(|&(_, at)| at % 1_000 == 0));
        assert_eq!((edges[1].1 - edges[0].1, edges[2].1 - edges[1].1), (9_000_000, 4_500_000), "the leader to the ns");
    }

    #[test]
    fn presses_play_a_frame_period_apart() {
        let mut ir = wired();
        ir.on_input("1");
        ir.on_input("2");
        let first = drain(&mut ir, 0);
        assert_eq!(first.len(), 68, "one frame at a time");
        assert!(drain(&mut ir, 0).is_empty(), "the second waits its turn in the part");
        let mut now = 0;
        let second = loop {
            now += 1_000_000;
            let got = drain(&mut ir, now);
            if !got.is_empty() {
                break got;
            }
        };
        assert!(second[0].1 >= first[0].1 + FRAME_PERIOD_NS, "starts a frame period after the first");
        assert!(second[0].1 < first[0].1 + FRAME_PERIOD_NS + 1_000_000, "and within a poll of it");
        assert!(second[0].1 - LEAD_IN_NS <= now + 1_000, "handed over a lead-in ahead");
        assert_eq!(decode(&second), Some((0xFF00, 0xB946)));
    }

    #[test]
    fn a_reset_inside_a_frame_releases_the_line_and_starts_afresh() {
        let mut ir = wired();
        ir.on_input("ok");
        let first = drain(&mut ir, 10_000_000);
        assert!(drain(&mut ir, first[3].1).is_empty(), "polled mid-frame");
        // The chip resets: its clock starts again near zero.
        let line = drain(&mut ir, 2_000);
        assert_eq!(line, [(true, 2_000)], "the line back high at once");
        ir.on_input("up");
        let next = drain(&mut ir, 3_000);
        assert_eq!(next[0].1, 3_000 + LEAD_IN_NS, "on the new clock, not a frame period after the old one");
        assert_eq!(ir.state().unwrap().get("frames"), Some("0"), "a frame cut short is not counted");
    }

    #[test]
    fn a_reset_after_a_frame_has_played_sends_nothing() {
        let mut ir = wired();
        ir.on_input("ok");
        let edges = drain(&mut ir, 0);
        assert!(drain(&mut ir, edges[67].1).is_empty());
        assert!(drain(&mut ir, 1_000).is_empty(), "the line is already high");
        assert_eq!(ir.state().unwrap().get("frames"), Some("1"));
    }

    #[test]
    fn it_claims_its_pin_and_ignores_what_it_does_not_have() {
        assert_eq!(wired().claims(), [drives(1 << PIN)]);
        let mut ir = wired();
        ir.on_input("power");
        assert!(drain(&mut ir, 0).is_empty(), "no such key");
        let mut unwired = Vs1838b::default();
        assert!(unwired.claims().is_empty());
        unwired.on_input("ok");
        assert!(unwired.poll(0).is_none(), "an unwired module has no line to move");
    }

    #[test]
    fn it_shows_the_last_key_played() {
        let mut ir = wired();
        let s = ir.state().unwrap();
        assert_eq!((s.get("key"), s.get("command"), s.get("frames")), (Some("none"), Some("none"), Some("0")));
        ir.on_input("down");
        let edges = drain(&mut ir, 0);
        let s = ir.state().unwrap();
        assert_eq!(s.get("frames"), Some("0"), "handed over is not played");
        drain(&mut ir, edges[67].1);
        let s = ir.state().unwrap();
        assert_eq!(
            (s.get("address"), s.get("command"), s.get("frames"), s.get("key")),
            (Some("0"), Some("82"), Some("1"), Some("down"))
        );
    }
}
