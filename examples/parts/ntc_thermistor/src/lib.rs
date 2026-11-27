//! An NTC thermistor in a voltage divider, written as a virtual part.
//!
//! A part is a WebAssembly module that models a component wired to the chip's
//! pins. This one is the common thermistor module: a 10 kΩ fixed resistor from
//! the 3.3 V rail to the output and a 10 kΩ (at 25 °C) NTC thermistor, B 3950,
//! from the output to ground. The output is on an ADC pad, so the firmware
//! reads a voltage that falls as the thermistor warms:
//!
//! ```text
//!   R(T) = R25 · exp(B · (1/T − 1/T25))        T in kelvin
//!   V    = 3300 mV · R(T) / (R_FIXED + R(T))   1650 mV at 25 °C
//! ```
//!
//! Set to a new temperature, it warms or cools toward it as a small bead
//! does in a stirred liquid: each second closes 63 % of the gap (a
//! one-second time constant). The part sends each such change as one
//! `AnalogWave`, the output a hundred times a second until it settles, and
//! moves the output at no other time.
//!
//! `Thermistor` is the device and knows nothing about WebAssembly. The
//! `export_part!` line at the end makes it a part.

use esp_part::prelude::*;

/// The module's own name for its output pin. The board says which chip pin
/// it is wired to, as `{ pins: { OUT: 3 } }`.
const OUT: &str = "OUT";

/// The rail across the divider, in millivolts.
const RAIL_MV: f64 = 3300.0;
/// The fixed resistor, from the rail to the output, in ohms.
const R_FIXED: f64 = 10_000.0;
/// The thermistor's resistance at 25 °C, in ohms, and its B constant (25/85).
const R25: f64 = 10_000.0;
const B: f64 = 3950.0;
/// 25 °C in kelvin.
const T25_K: f64 = 298.15;
/// The range the thermistor is specified over, in °C. A temperature outside
/// it reads as its nearest end.
const MIN_C: f64 = -40.0;
const MAX_C: f64 = 125.0;
/// The thermal time constant, in nanoseconds: a change of temperature is
/// 63 % done after it.
const TAU_NS: f64 = 1e9;
/// How far apart the output's voltages are along a change, in nanoseconds.
const STEP_NS: u64 = 10_000_000;
/// The most voltages one change sends. The largest, from one end of the
/// range to the other, settles in under ten seconds, a thousand of them.
const RAMP_MAX: u64 = 2000;

#[derive(Default)]
struct Thermistor {
    /// The chip pin the output is wired to. `None` until the board says, or
    /// when it left the pin unconnected.
    pin: Option<u32>,
    /// The temperature it was set to and moves toward, in °C.
    target_c: f64,
    /// Where the present change started, in °C, and when, on the emulated
    /// clock. `None` before the first change.
    from: Option<(f64, u64)>,
    /// A temperature set since the last poll, in °C.
    next_c: Option<f64>,
    /// The output was held at start-up.
    started: bool,
    /// The emulated time of the last poll. The chip's clock starting again
    /// below it is a chip reset.
    last_ns: u64,
}

/// The divider's output at `temp_c`, in millivolts.
fn millivolts(temp_c: f64) -> u32 {
    let r = R25 * (B * (1.0 / (temp_c + 273.15) - 1.0 / T25_K)).exp();
    (RAIL_MV * r / (R_FIXED + r)).round() as u32
}

impl Thermistor {
    /// The thermistor's temperature at `t_ns`, in °C.
    fn temp_at(&self, t_ns: u64) -> f64 {
        match self.from {
            Some((from_c, since)) => {
                let dt = t_ns.saturating_sub(since) as f64;
                self.target_c + (from_c - self.target_c) * (-dt / TAU_NS).exp()
            }
            None => self.target_c,
        }
    }

    /// The output from `now_ns` on, `STEP_NS` apart, until it settles at the
    /// target's voltage, which it ends on.
    fn ramp(&self, now_ns: u64) -> Vec<u32> {
        let end = millivolts(self.target_c);
        let mut mvs = Vec::new();
        for k in 0..RAMP_MAX - 1 {
            let mv = millivolts(self.temp_at(now_ns + k * STEP_NS));
            mvs.push(mv);
            if mv == end {
                return mvs;
            }
        }
        mvs.push(end);
        mvs
    }
}

impl Device for Thermistor {
    const NAME: &'static str = "ntc_thermistor";

    /// Where the board wired the part. It starts at 25 °C.
    fn configure(&mut self, config: &PartConfig) {
        self.pin = config.pins.get(OUT).copied();
        self.target_c = 25.0;
    }

    /// The pad the output is on.
    fn claims(&self) -> Vec<Claim> {
        self.pin.map(analog).into_iter().collect()
    }

    /// The output at start-up, held, and each change of temperature from the
    /// moment it was set, as one run of voltages. A chip reset restarts the
    /// chip's clock and drops the rest of a run it cut short, so the part
    /// sends the rest again from where the change had got to.
    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        let pin = self.pin?;
        let reset = now_ns < self.last_ns;
        let cut_short = reset && millivolts(self.temp_at(self.last_ns)) != millivolts(self.target_c);
        if cut_short {
            self.from = Some((self.temp_at(self.last_ns), now_ns));
        }
        let set = self.next_c.take();
        if let Some(next) = set {
            self.from = Some((self.temp_at(now_ns), now_ns));
            self.target_c = next;
        }
        self.last_ns = now_ns;
        if set.is_some() || cut_short {
            self.started = true;
            return Some(wave(pin, STEP_NS, self.ramp(now_ns), now_ns));
        }
        if !self.started {
            self.started = true;
            return Some(hold(pin, millivolts(self.target_c), now_ns));
        }
        None
    }

    /// Somebody warmed or cooled the thermistor. `set:temperature=<°C>` sets
    /// the temperature it moves toward; `warmer` and `cooler` move that a
    /// degree.
    fn on_input(&mut self, name: &str) {
        let set = self.next_c.unwrap_or(self.target_c);
        let t = match set_value(name) {
            Some(("temperature", value)) => value,
            Some(_) => return, // Not a value this part has.
            None => match name {
                "warmer" => set + 1.0,
                "cooler" => set - 1.0,
                _ => return, // Not an interaction this part has.
            },
        };
        self.next_c = Some(t.clamp(MIN_C, MAX_C));
    }

    /// The temperature it was set to, and the output it settles at.
    fn state(&self) -> Option<State> {
        let set = self.next_c.unwrap_or(self.target_c);
        Some(State::new().set("temperature", set).set("millivolts", millivolts(set)))
    }
}

// The longest change is 2000 voltages of two bytes, and the message around
// them under 30 more.
esp_part::export_part!(Thermistor, buffer = 4096);

#[cfg(test)]
mod tests {
    use super::*;

    /// Wired as README.md's board wires it: the output on GPIO 3.
    fn sensor() -> Thermistor {
        let mut t = Thermistor::default();
        t.configure(&PartConfig { pins: [(OUT.to_string(), 3)].into(), ..Default::default() });
        t
    }

    /// What one poll at `now` sends: the pin and the voltages, one for a hold.
    fn sent(t: &mut Thermistor, now: u64) -> Option<(u32, Vec<u32>)> {
        let msg = t.poll(now)?;
        assert_eq!(msg.sim_time_ns, now, "from the poll's own time");
        match msg.payload? {
            esp_part::Payload::AnalogDrive(a) => Some((a.pin, vec![a.millivolts])),
            esp_part::Payload::AnalogWave(w) => {
                assert_eq!(w.period_ns, STEP_NS);
                Some((w.pin, w.millivolts))
            }
            other => panic!("not a voltage: {other:?}"),
        }
    }

    const S: u64 = 1_000_000_000;

    #[test]
    fn at_25_c_the_divider_is_half_the_rail() {
        let mut t = sensor();
        assert_eq!(t.claims(), vec![analog(3)]);
        assert_eq!(sent(&mut t, 0), Some((3, vec![1650])));
        assert_eq!(sent(&mut t, 0), None, "said once, then held");
        assert_eq!(sent(&mut t, S), None, "nothing moves it by itself");
    }

    #[test]
    fn warming_it_lowers_the_output_to_where_it_settles() {
        // B 3950: R(50 °C) = 3588 Ω, R(0 °C) = 33 621 Ω.
        let mut t = sensor();
        sent(&mut t, 0);
        t.on_input("set:temperature=50");
        assert_eq!(sent(&mut t, S).unwrap().1.last(), Some(&871));
        t.on_input("set:temperature=0");
        assert_eq!(sent(&mut t, 20 * S).unwrap().1.last(), Some(&2543));
        t.on_input("warmer");
        assert!(*sent(&mut t, 40 * S).unwrap().1.last().unwrap() < 2543);
        t.on_input("set:temperature=500");
        sent(&mut t, 60 * S);
        assert_eq!(t.target_c, MAX_C, "held to the specified range");
        t.on_input("wiggle");
        assert_eq!(sent(&mut t, 80 * S), None);
    }

    #[test]
    fn a_new_temperature_ramps_the_output_from_where_it_is() {
        let mut t = sensor();
        sent(&mut t, 0);
        t.on_input("set:temperature=50");
        let (pin, mvs) = sent(&mut t, S).unwrap();
        assert_eq!(pin, 3);
        assert_eq!(mvs[0], 1650, "no step: it starts at 25 °C");
        assert!(mvs.windows(2).all(|w| w[1] <= w[0]), "falls all the way as it warms");
        // One second in, 63 % of the way: 25 + 25 · (1 − 1/e) °C.
        assert_eq!(mvs[100], millivolts(25.0 + 25.0 * (1.0 - (-1.0f64).exp())));
        assert!(mvs.len() < 1000, "settled within ten seconds, {} voltages", mvs.len());
    }

    #[test]
    fn a_temperature_set_during_a_change_starts_from_where_the_change_got_to() {
        let mut t = sensor();
        sent(&mut t, 0);
        t.on_input("set:temperature=50");
        let (_, first) = sent(&mut t, S).unwrap();
        t.on_input("set:temperature=0");
        let (_, second) = sent(&mut t, 2 * S).unwrap();
        assert_eq!(second[0], first[100], "one second into the first change");
        assert_eq!(second.last(), Some(&2543));
    }

    #[test]
    fn a_chip_reset_during_a_change_sends_the_rest_of_it() {
        let mut t = sensor();
        sent(&mut t, 0);
        t.on_input("set:temperature=50");
        let (_, before) = sent(&mut t, 10 * S).unwrap();
        assert_eq!(sent(&mut t, 10 * S + S / 2), None, "half a second in");
        // The chip reset: its clock starts again.
        let (_, rest) = sent(&mut t, 1_000_000).unwrap();
        assert_eq!(rest[0], before[50], "from where the change had got to");
        assert_eq!(rest.last(), Some(&871));
        // A reset after the change settled leaves the held voltage alone.
        assert_eq!(sent(&mut t, 30 * S), None);
        assert_eq!(sent(&mut t, 2_000_000), None);
    }

    #[test]
    fn the_largest_change_fits_the_parts_buffer() {
        use esp_part::prost::Message;
        let mut t = sensor();
        sent(&mut t, 0);
        for (temp, at) in [("-40", S), ("125", 30 * S), ("-40", 60 * S)] {
            t.on_input(&format!("set:temperature={temp}"));
            let msg = t.poll(at).unwrap();
            assert!(msg.encoded_len() <= PART_BUFFER, "{} bytes to {temp} °C", msg.encoded_len());
        }
    }

    #[test]
    fn an_unwired_module_claims_and_holds_nothing() {
        let mut t = Thermistor::default();
        t.configure(&PartConfig::default());
        assert!(t.claims().is_empty());
        assert_eq!(sent(&mut t, 0), None);
    }
}
