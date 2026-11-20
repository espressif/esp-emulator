//! A WS2812 (NeoPixel) LED strip, written as a virtual part.
//!
//! The strip reads one line, DIN. Each bit is a high then a low, about
//! 1.25 us in all: a high of about 0.4 us is a 0, of about 0.8 us a 1. Each
//! pixel takes 24 bits, green, red and blue, most significant bit first, and
//! a low of 50 us or more latches the frame: every pixel shows its colour.
//!
//! The part reads DIN as runs of edges ([`reads_edges`]), each a first level
//! and the widths after it, and decodes every high by its width against
//! 0.6 us. A frame ends at the first of: a low of 50 us or more inside a run;
//! a run whose first edge comes 50 us or more after the line went low; a call
//! whose time is 50 us or more past the last edge, the line low. Runs come at
//! most a heartbeat after their edges, so a frame shows within a millisecond
//! of its end.

use esp_part::prelude::*;

/// The strip's data input, as printed on it. The board says which chip pin it
/// is wired to, as `{ pins: { DIN: 8 } }`.
const DIN: &str = "DIN";

/// A high longer than this is a 1: between the 0.4 us and 0.8 us the
/// datasheet gives, each with 150 ns of margin.
const ONE_MIN_NS: u64 = 600;

/// The low that latches a frame.
const LATCH_NS: u64 = 50_000;

/// The longest strip the part decodes; bits past it are dropped.
const MAX_PIXELS: usize = 256;

#[derive(Default)]
struct Ws2812 {
    /// The chip pin `DIN` is wired to; `None` where the board left it out.
    din: Option<u32>,
    /// The frame being received, a bit a high.
    bits: Vec<bool>,
    /// What the strip shows: the last frame latched, red, green, blue.
    pixels: Vec<[u8; 3]>,
    /// Frames latched.
    frames: u64,
    /// The line's level after the last edge, and when that edge was.
    level: bool,
    since: Option<u64>,
}

impl Ws2812 {
    /// A level of `width_ns` ended: a high is a bit, a long enough low ends
    /// the frame.
    fn level_ended(&mut self, high: bool, width_ns: u64) {
        if high {
            if self.bits.len() < MAX_PIXELS * 24 {
                self.bits.push(width_ns > ONE_MIN_NS);
            }
        } else if width_ns >= LATCH_NS {
            self.latch();
        }
    }

    /// Show the frame received: whole pixels only, a part pixel dropped.
    fn latch(&mut self) {
        if self.bits.is_empty() {
            return;
        }
        let byte = |bits: &[bool]| bits.iter().fold(0u8, |b, &bit| b << 1 | bit as u8);
        self.pixels = self
            .bits
            .chunks_exact(24)
            .map(|p| {
                let (g, r, b) = (byte(&p[..8]), byte(&p[8..16]), byte(&p[16..]));
                [r, g, b]
            })
            .collect();
        self.frames += 1;
        self.bits.clear();
    }

    /// The line has been low since the last edge: long enough, and the frame
    /// it ended is shown.
    fn at(&mut self, now_ns: u64) {
        if let Some(since) = self.since {
            if !self.level && now_ns.saturating_sub(since) >= LATCH_NS {
                self.latch();
            }
        }
    }
}

impl Device for Ws2812 {
    const NAME: &'static str = "ws2812";
    const ON_MESSAGE: bool = true;

    fn configure(&mut self, config: &PartConfig) {
        self.din = config.pins.get(DIN).copied();
    }

    /// `DIN`, read as runs of edges.
    fn claims(&self) -> Vec<Claim> {
        self.din.map(|pin| vec![reads_edges(1 << pin)]).unwrap_or_default()
    }

    fn on_edges(&mut self, pin: u32, first_level: bool, widths_ns: &[u32], at_ns: u64) {
        if Some(pin) != self.din {
            return;
        }
        // The level before the run lasted from the last edge to its first.
        if let Some(since) = self.since {
            self.level_ended(self.level, at_ns.saturating_sub(since));
        }
        let mut t = at_ns;
        let mut level = first_level;
        for &w in widths_ns {
            self.level_ended(level, w as u64);
            t += w as u64;
            level = !level;
        }
        self.level = level;
        self.since = Some(t);
    }

    fn on_pins(&mut self, _levels: u64, now_ns: u64) {
        self.at(now_ns);
    }

    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        self.at(now_ns);
        None
    }

    /// `frames` latched, how many `pixels`, and each pixel by its index as
    /// `#rrggbb`.
    fn state(&self) -> Option<State> {
        let mut s = State::new().set("frames", self.frames).set("pixels", self.pixels.len());
        for (i, [r, g, b]) in self.pixels.iter().enumerate() {
            s = s.set(&i.to_string(), format!("#{r:02x}{g:02x}{b:02x}"));
        }
        Some(s)
    }
}

// A run of edges is up to 2,581 bytes (`PART_ABI.md`).
esp_part::export_part!(Ws2812, buffer = 4096);

#[cfg(test)]
mod tests {
    use super::*;

    const PIN: u32 = 8;

    fn wired() -> Ws2812 {
        let mut s = Ws2812::default();
        s.configure(&PartConfig { pins: [(DIN.to_string(), PIN)].into(), ..Default::default() });
        s
    }

    /// The widths of `pixels` on the wire, from the first rise, as the
    /// strip test firmware sends them: a 0 is 400 ns high and 850 ns low, a 1
    /// 800 ns high and 450 ns low; the last low is `tail_ns`.
    fn wire(pixels: &[[u8; 3]], tail_ns: u32) -> Vec<u32> {
        let mut w = Vec::new();
        for &[r, g, b] in pixels {
            for byte in [g, r, b] {
                for bit in (0..8).rev() {
                    let one = byte >> bit & 1 == 1;
                    w.extend([if one { 800 } else { 400 }, if one { 450 } else { 850 }]);
                }
            }
        }
        *w.last_mut().unwrap() = tail_ns;
        w
    }

    fn shown(s: &Ws2812) -> Vec<String> {
        let st = s.state().unwrap();
        (0..s.pixels.len()).map(|i| st.get(&i.to_string()).unwrap().to_string()).collect()
    }

    const FRAME: [[u8; 3]; 4] = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [1, 2, 3]];

    #[test]
    fn a_frame_shows_once_a_long_low_ends_it() {
        let mut s = wired();
        let w = wire(&FRAME, 60_000);
        // The 60 us tail is a width, so the run ends it: the next rise.
        s.on_edges(PIN, true, &w, 1_000);
        assert_eq!(shown(&s), ["#ff0000", "#00ff00", "#0000ff", "#010203"]);
        assert_eq!(s.state().unwrap().get("frames"), Some("1"));
    }

    #[test]
    fn a_frame_whose_end_is_the_last_edge_shows_when_time_passes() {
        let mut s = wired();
        let mut w = wire(&FRAME, 0);
        w.pop(); // the run ends on the last fall: the low after it is open
        s.on_edges(PIN, true, &w, 1_000);
        let end = 1_000 + w.iter().map(|&x| x as u64).sum::<u64>();
        s.on_pins(0, end);
        assert!(s.pixels.is_empty(), "not yet: the low has only begun");
        assert!(s.poll(end + 49_000).is_none());
        assert!(s.pixels.is_empty());
        s.poll(end + 50_000);
        assert_eq!(shown(&s), ["#ff0000", "#00ff00", "#0000ff", "#010203"]);
    }

    #[test]
    fn a_frame_in_pieces_decodes_as_one_wherever_it_is_cut() {
        let w = wire(&FRAME, 60_000);
        for cut in [1, 2, 47, 48, 95] {
            let mut s = wired();
            let (head, tail) = w.split_at(cut);
            let start = 1_000u64;
            s.on_edges(PIN, true, &head[..cut - 1], start);
            // The next run starts with the edge that ends the head's last level.
            let at = start + head.iter().map(|&x| x as u64).sum::<u64>();
            s.on_edges(PIN, cut % 2 == 0, tail, at);
            assert_eq!(shown(&s), ["#ff0000", "#00ff00", "#0000ff", "#010203"], "cut after {cut} widths");
        }
    }

    #[test]
    fn a_gap_between_runs_ends_the_frame_before_the_next() {
        let mut s = wired();
        let mut w = wire(&FRAME, 0);
        w.pop();
        s.on_edges(PIN, true, &w, 1_000);
        let end = 1_000 + w.iter().map(|&x| x as u64).sum::<u64>();
        let next = wire(&[[9, 8, 7]], 60_000);
        s.on_edges(PIN, true, &next, end + 300_000);
        assert_eq!(s.state().unwrap().get("frames"), Some("2"));
        assert_eq!(shown(&s), ["#090807"]);
    }

    #[test]
    fn a_part_pixel_is_dropped() {
        // One whole pixel and 20 bits of the next, then the latching low.
        let mut s = wired();
        let mut w = wire(&[[4, 5, 6]], 850);
        w.extend(&wire(&[[1, 2, 3]], 0)[..39]);
        w.push(60_000);
        s.on_edges(PIN, true, &w, 0);
        assert_eq!(shown(&s), ["#040506"]);
    }

    #[test]
    fn it_reads_its_pin_as_edges_and_an_unwired_one_does_nothing() {
        assert_eq!(wired().claims(), [reads_edges(1 << PIN)]);
        let mut s = Ws2812::default();
        assert!(s.claims().is_empty());
        s.on_edges(PIN, true, &wire(&FRAME, 60_000), 0);
        assert!(s.pixels.is_empty());
        let mut other = wired();
        other.on_edges(PIN + 1, true, &wire(&FRAME, 60_000), 0);
        assert!(other.pixels.is_empty(), "another pin's run");
    }
}
