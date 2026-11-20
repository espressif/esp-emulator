//! A DHT22 (AM2302) temperature and humidity sensor, written as a virtual
//! part.
//!
//! The sensor sits on one data line with a pull-up. The host pulls the line
//! low for at least 1 ms and lets go; 20 to 40 us later the sensor answers
//! with an 80 us low and an 80 us high, then 40 bits, each a 50 us low and a
//! high of 26 us for a 0 or 70 us for a 1, then a last 50 us low before it
//! lets go of the line. The bits are the relative humidity and the
//! temperature in tenths, 16 bits each, most significant first, the
//! temperature's top bit its sign, then a checksum: the low byte of the sum
//! of the other four bytes.
//!
//! The host's start pulse arrives through `on_pins`, whose levels are the line
//! as the chip drives it and never what the part drives, so the reply is never
//! taken for a request. The reply goes out as one waveform ([`play`]) stamped
//! 30 us after the release: natively it lands to the tick, and a part in a
//! browser, which answers after the emulator has moved on, has its reply
//! start at once with every width kept.

use esp_part::prelude::*;

/// The sensor's data pin, as printed on it. The board says which chip pin it
/// is wired to, as `{ pins: { DATA: 5 } }`.
const DATA: &str = "DATA";

/// The shortest low taken for a start pulse. The datasheet asks the host for
/// 1 ms; the sensor answers shorter ones, and so does the part.
const START_MIN_NS: u64 = 500_000;

/// From the host letting go to the reply: the middle of the datasheet's 20 to
/// 40 us.
const RESPONSE_NS: u64 = 30_000;

/// The reply's levels: each half of the presence pulse, a bit's low, and a
/// bit's high for a 0 and for a 1.
const PRESENCE_NS: u32 = 80_000;
const BIT_LOW_NS: u32 = 50_000;
const ZERO_HIGH_NS: u32 = 26_000;
const ONE_HIGH_NS: u32 = 70_000;

/// The datasheet's ranges, in tenths of a percent and of a degree.
const HUMIDITY: (i32, i32) = (0, 999);
const TEMPERATURE: (i32, i32) = (-400, 800);

struct Dht22 {
    /// The chip pin `DATA` is wired to; `None` where the board left it out.
    data: Option<u32>,
    /// The reading, in tenths of a percent and of a degree.
    humidity: i32,
    temperature: i32,
    /// The line as the chip last drove it; `None` before the first word.
    line: Option<bool>,
    /// When the host pulled the line low.
    low_since: Option<u64>,
    /// A reply owed, and when it starts.
    reply_at: Option<u64>,
    /// The end of the reply playing: the line moving before then is not a
    /// request.
    busy_until: u64,
    /// The latest time seen: a clock that goes back is a chip reset.
    last_now: u64,
    /// Replies handed over.
    reads: u64,
}

impl Default for Dht22 {
    fn default() -> Self {
        Self {
            data: None,
            humidity: 450,
            temperature: 235,
            line: None,
            low_since: None,
            reply_at: None,
            busy_until: 0,
            last_now: 0,
            reads: 0,
        }
    }
}

/// The five bytes of a reading: humidity, temperature (sign and magnitude),
/// checksum.
fn frame(humidity: i32, temperature: i32) -> [u8; 5] {
    let rh = humidity as u16;
    let t = temperature.unsigned_abs() as u16 | if temperature < 0 { 0x8000 } else { 0 };
    let b = [(rh >> 8) as u8, rh as u8, (t >> 8) as u8, t as u8];
    [b[0], b[1], b[2], b[3], b.iter().fold(0u8, |sum, &x| sum.wrapping_add(x))]
}

/// The reply carrying `bytes`, as widths from its first low: 83 of them,
/// ending on the line let go.
fn reply(bytes: [u8; 5]) -> Vec<u32> {
    let mut widths = vec![PRESENCE_NS, PRESENCE_NS];
    for byte in bytes {
        for bit in (0..8).rev() {
            widths.extend([BIT_LOW_NS, if byte >> bit & 1 == 1 { ONE_HIGH_NS } else { ZERO_HIGH_NS }]);
        }
    }
    widths.push(BIT_LOW_NS);
    widths
}

/// A value in tenths as the part shows it: `-3.5`, `45.0`.
fn tenths(v: i32) -> String {
    format!("{}{}.{}", if v < 0 { "-" } else { "" }, v.abs() / 10, v.abs() % 10)
}

impl Dht22 {
    /// Notice the clock. A chip reset starts it again: a request or a reply
    /// on the old clock is void, and the emulator has put the line back at
    /// the level the reply ends on.
    fn clock(&mut self, now_ns: u64) {
        if now_ns < self.last_now {
            self.low_since = None;
            self.reply_at = None;
            self.busy_until = 0;
        }
        self.last_now = now_ns;
    }
}

impl Device for Dht22 {
    const NAME: &'static str = "dht22";

    fn configure(&mut self, config: &PartConfig) {
        self.data = config.pins.get(DATA).copied();
    }

    /// `DATA`: driven for the reply, and watched for the host's start pulse
    /// at the time each edge happens.
    fn claims(&self) -> Vec<Claim> {
        self.data.map(|pin| vec![drives(1 << pin), watches(1 << pin)]).unwrap_or_default()
    }

    fn on_pins(&mut self, levels: u64, now_ns: u64) {
        self.clock(now_ns);
        let Some(pin) = self.data else { return };
        let high = levels >> pin & 1 == 1;
        // The first word is where the line rests, not an edge.
        let Some(was) = self.line.replace(high) else { return };
        if was == high || now_ns < self.busy_until {
            return;
        }
        if !high {
            self.low_since = Some(now_ns);
        } else if self.low_since.take().is_some_and(|since| now_ns - since >= START_MIN_NS) {
            self.reply_at = Some(now_ns + RESPONSE_NS);
        }
    }

    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        self.clock(now_ns);
        let pin = self.data?;
        let at = self.reply_at.take()?;
        let widths = reply(frame(self.humidity, self.temperature));
        self.busy_until = at.max(now_ns) + widths.iter().map(|&w| w as u64).sum::<u64>();
        self.reads += 1;
        Some(play(pin, false, &widths, at))
    }

    /// `set:humidity=V` in percent and `set:temperature=V` in degrees
    /// Celsius, to a tenth, held to the datasheet's range.
    fn on_input(&mut self, name: &str) {
        let Some((what, value)) = set_value(name) else { return };
        let v = (value * 10.0).round() as i32;
        match what {
            "humidity" => self.humidity = v.clamp(HUMIDITY.0, HUMIDITY.1),
            "temperature" => self.temperature = v.clamp(TEMPERATURE.0, TEMPERATURE.1),
            _ => {}
        }
    }

    fn state(&self) -> Option<State> {
        Some(
            State::new()
                .set("humidity", tenths(self.humidity))
                .set("reads", self.reads)
                .set("temperature", tenths(self.temperature)),
        )
    }
}

esp_part::export_part!(Dht22);

#[cfg(test)]
mod tests {
    use super::*;
    use esp_part::prost::Message;
    use esp_part::Payload;

    const PIN: u32 = 5;
    const RELEASED: u64 = u64::MAX;

    fn wired() -> Dht22 {
        let mut d = Dht22::default();
        d.configure(&PartConfig { pins: [(DATA.to_string(), PIN)].into(), ..Default::default() });
        d.on_pins(RELEASED, 0);
        d
    }

    /// The host's start pulse: low at `at_ns` for `low_ns`, then let go.
    /// Returns the release.
    fn ask(d: &mut Dht22, at_ns: u64, low_ns: u64) -> u64 {
        d.on_pins(RELEASED & !(1 << PIN), at_ns);
        d.on_pins(RELEASED, at_ns + low_ns);
        at_ns + low_ns
    }

    /// The reply handed over at `now_ns`: its stamp and widths.
    fn answer(d: &mut Dht22, now_ns: u64) -> Option<(u64, Vec<u32>)> {
        let msg = d.poll(now_ns)?;
        match msg.payload {
            Some(Payload::PulseDrive(w)) => {
                assert_eq!((w.pin, w.first_level), (PIN, false), "a low on DATA first");
                Some((msg.sim_time_ns, w.widths_ns))
            }
            other => panic!("expected a PulseDrive, got {other:?}"),
        }
    }

    /// A host's decode: the presence pulse, then each bit by its high against
    /// the midpoint of 26 and 70 us.
    fn decode(widths: &[u32]) -> [u8; 5] {
        assert_eq!(widths.len(), 83, "presence, 40 bits and the last low");
        assert_eq!(&widths[..2], &[PRESENCE_NS, PRESENCE_NS]);
        let mut out = [0u8; 5];
        for (i, pair) in widths[2..82].chunks(2).enumerate() {
            assert_eq!(pair[0], BIT_LOW_NS);
            out[i / 8] = out[i / 8] << 1 | (pair[1] > 48_000) as u8;
        }
        out
    }

    #[test]
    fn a_start_pulse_is_answered_30_us_after_the_release_in_one_waveform() {
        let mut d = wired();
        let released = ask(&mut d, 1_000_000, 2_000_000);
        let (at, widths) = answer(&mut d, released + 1_600).expect("a reply");
        assert_eq!(at, released + RESPONSE_NS);
        assert_eq!(decode(&widths), frame(450, 235));
        assert_eq!(widths.len() % 2, 1, "an odd count of widths ends on the line let go");
        assert!(answer(&mut d, released + 2_000).is_none(), "one reply a request");
    }

    #[test]
    fn the_bytes_carry_the_reading_and_its_checksum() {
        assert_eq!(frame(450, 235), [0x01, 0xC2, 0x00, 0xEB, 0xAE]);
        assert_eq!(frame(652, -101), [0x02, 0x8C, 0x80, 0x65, 0x73], "the sign in the top bit");
    }

    #[test]
    fn a_low_shorter_than_a_start_pulse_is_no_request() {
        let mut d = wired();
        let released = ask(&mut d, 1_000_000, 100_000);
        assert!(answer(&mut d, released).is_none());
    }

    #[test]
    fn the_line_moving_while_the_reply_plays_is_no_request() {
        let mut d = wired();
        let released = ask(&mut d, 1_000_000, 2_000_000);
        let (at, widths) = answer(&mut d, released).unwrap();
        let end = at + widths.iter().map(|&w| w as u64).sum::<u64>();
        ask(&mut d, at + 100_000, 600_000);
        assert!(answer(&mut d, at + 700_000).is_none(), "inside the reply");
        let again = ask(&mut d, end + 1_000_000, 1_000_000);
        assert_eq!(answer(&mut d, again).map(|(at, _)| at), Some(again + RESPONSE_NS), "after it");
        assert_eq!(d.state().unwrap().get("reads"), Some("2"));
    }

    #[test]
    fn a_late_poll_still_stamps_the_reply_for_its_time() {
        // A browser part is polled after the emulator has moved on; the
        // emulator starts a late waveform at once with every width kept.
        let mut d = wired();
        let released = ask(&mut d, 1_000_000, 2_000_000);
        let (at, _) = answer(&mut d, released + 5_000_000).unwrap();
        assert_eq!(at, released + RESPONSE_NS);
    }

    #[test]
    fn a_reset_voids_a_reply_owed() {
        let mut d = wired();
        ask(&mut d, 10_000_000, 2_000_000);
        assert!(answer(&mut d, 1_000).is_none(), "the clock went back: a chip reset");
        let released = ask(&mut d, 2_000, 1_000_000);
        assert!(answer(&mut d, released).is_some(), "the next request on the new clock");
    }

    #[test]
    fn the_reading_is_set_by_input_and_held_to_range() {
        let mut d = wired();
        d.on_input("set:temperature=-3.46");
        d.on_input("set:humidity=120");
        d.on_input("set:pressure=1");
        let s = d.state().unwrap();
        assert_eq!((s.get("temperature"), s.get("humidity")), (Some("-3.5"), Some("99.9")));
        let released = ask(&mut d, 1_000_000, 2_000_000);
        assert_eq!(decode(&answer(&mut d, released).unwrap().1), frame(999, -35));
    }

    #[test]
    fn it_drives_and_watches_its_pin_and_an_unwired_one_does_nothing() {
        assert_eq!(wired().claims(), [drives(1 << PIN), watches(1 << PIN)]);
        let mut d = Dht22::default();
        assert!(d.claims().is_empty());
        d.on_pins(0, 1_000);
        d.on_pins(RELEASED, 3_000_000);
        assert!(d.poll(3_000_000).is_none());
    }

    #[test]
    fn the_reply_fits_the_parts_buffer() {
        let mut d = wired();
        let released = ask(&mut d, u64::MAX / 2, 2_000_000);
        assert!(d.poll(released).unwrap().encoded_len() < 512);
    }
}
