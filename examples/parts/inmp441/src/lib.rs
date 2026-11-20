//! An InvenSense INMP441 I2S MEMS microphone, written as a virtual part.
//!
//! A part is a WebAssembly module that models a component wired to the chip's
//! pins. This one drives the chip's I2S data input. In each frame of 64 SCK
//! cycles the microphone sends one 24-bit two's complement sample, MSB first,
//! in the half its L/R pin selects (low: the left, while WS is low; high: the
//! right), and leaves the other half undriven, which the receiver reads as
//! zeros. The "INMP441 Omnidirectional Microphone with Bottom Port and I2S
//! Digital Output" datasheet (InvenSense) is the reference; README.md lists
//! what this leaves out.
//!
//! What it hears is a sine tone, whose pitch and level a person sets, or
//! the sound its host feeds it (`part_audio_in`), such as a person's own
//! microphone in a page. The first sound fed switches it from the tone; the
//! input `tone` goes back to the tone and keeps it there, dropping what it is
//! fed, until the input `feed`. The chip learns nothing of that but the
//! samples.
//!
//! `Inmp441` is the device and knows nothing about WebAssembly. The
//! `export_part!` line at the end makes it a part. It paces itself in emulated
//! time, as `proto/PART_ABI.md` says a part that supplies samples does: it
//! counts from the receive format's time and keeps [`LEAD_NS`] queued ahead of
//! the time it is polled with.

use std::collections::VecDeque;
use std::f64::consts::TAU;

use esp_part::prelude::*;

/// The microphone's own names for its pins (the datasheet's pin table). The
/// board says which chip pins they are wired to, as `{ pins: { SD: 7 } }`.
const SD: &str = "SD";
const LR: &str = "L/R";

/// How far ahead of emulated time the samples are kept. The emulator holds
/// what arrives early, and the receiver takes it at the format's rate.
const LEAD_NS: u64 = 50_000_000;
/// The shared buffer, and what of it a message's own fields take: the
/// samples of one message are what fits the rest.
const BUFFER: usize = 16 * 1024;
const ENVELOPE: usize = 32;

/// The pitch a person can set, the 60 Hz to 15 kHz the datasheet gives as the
/// microphone's frequency response, and the level, 0 to -90 dBFS at the peak.
const MIN_HZ: u32 = 60;
const MAX_HZ: u32 = 15_000;
const MIN_DBFS: i32 = -90;
/// What `louder` and `quieter` move the level by.
const STEP_DB: i32 = 6;
/// How much of what it is fed it keeps, in ms of the stream: past that the
/// oldest goes, since a sound heard late is only delay.
const HELD_MS: u64 = 250;

struct Inmp441 {
    /// The chip pins the microphone's SD and L/R are wired to. `None` until
    /// the board says, or when it left the pin unconnected.
    sd: Option<u32>,
    lr: Option<u32>,
    /// Whether L/R is high: the right half of the frame. Unwired is low.
    right: bool,
    /// The tone it hears: its pitch, and its peak in dBFS.
    tone_hz: u32,
    level_dbfs: i32,
    /// The receive format, once the chip has said it.
    frame: Option<audio::Frame>,
    /// When the stream being supplied started, and how many frames of it
    /// have been sent.
    start_ns: u64,
    sent: u64,
    /// Where in a cycle of the tone the next sample is, in turns.
    phase: f64,
    /// The latest time it was told: an earlier one means the chip reset.
    last_ns: u64,
    /// Whether it hears what its host feeds it rather than its tone.
    fed: bool,
    /// Whether a person chose the tone: what it is fed is dropped until they
    /// choose the feed.
    tone_held: bool,
    /// What it was fed and has not supplied yet, as 16-bit samples.
    heard: VecDeque<i16>,
}

impl Default for Inmp441 {
    fn default() -> Self {
        Self {
            sd: None,
            lr: None,
            right: false,
            tone_hz: 440,
            level_dbfs: -12,
            frame: None,
            start_ns: 0,
            sent: 0,
            phase: 0.0,
            last_ns: 0,
            fed: false,
            tone_held: false,
            heard: VecDeque::new(),
        }
    }
}

impl Inmp441 {
    /// Note the time. One earlier than the last means the chip reset: the
    /// receiver it was supplying is gone, so it waits for the next format.
    fn at(&mut self, now_ns: u64) {
        if now_ns < self.last_ns {
            self.frame = None;
            self.heard.clear();
        }
        self.last_ns = now_ns;
    }

    /// When the samples sent so far run out.
    fn sent_until(&self, frame: audio::Frame) -> u64 {
        let ns = u128::from(self.sent) * 1_000_000_000 / u128::from(frame.rate_hz);
        self.start_ns + ns as u64
    }

    /// `frames` frames of the tone, in `frame`'s layout: the sample in the
    /// slot L/R selects, zeros in the others.
    fn tone(&mut self, frame: audio::Frame, frames: usize) -> Vec<u8> {
        let mut pcm = vec![0; frames * frame.bytes()];
        let slot = if self.right && frame.slots > 1 { 1 } else { 0 };
        // The decimation filter removes what the frame rate cannot carry.
        let audible = f64::from(self.tone_hz) < f64::from(frame.rate_hz) / 2.0;
        let peak = f64::from(i32::MAX) * 10f64.powf(f64::from(self.level_dbfs) / 20.0);
        let step = f64::from(self.tone_hz) / f64::from(frame.rate_hz);
        for f in pcm.chunks_exact_mut(frame.bytes()) {
            if self.fed {
                // What it was fed, its 16 bits the top of the 24; silence
                // once the feed has run dry, as in a quiet room.
                let sample = i32::from(self.heard.pop_front().unwrap_or(0)) << 16;
                frame.write(f, slot, sample);
            } else if audible {
                // 24 bits, left-aligned in 32: the low byte is never driven.
                let sample = (peak * (self.phase * TAU).sin()) as i32 & !0xFF;
                frame.write(f, slot, sample);
            }
            self.phase = (self.phase + step).fract();
        }
        pcm
    }
}

impl Device for Inmp441 {
    const NAME: &'static str = "inmp441";
    const ON_MESSAGE: bool = true;

    /// Where the board wired the part.
    fn configure(&mut self, config: &PartConfig) {
        self.sd = config.pins.get(SD).copied();
        self.lr = config.pins.get(LR).copied();
    }

    /// Its data output, SD, is the chip's data input. A microphone the board
    /// left without SD still claims, with no pad, so the emulator refuses
    /// the claim and says so.
    fn claims(&self) -> Vec<Claim> {
        vec![i2s(None, self.sd)]
    }

    /// L/R is a strap: its level picks the half of the frame.
    fn on_pins(&mut self, levels: u64, now_ns: u64) {
        self.at(now_ns);
        if let Some(lr) = self.lr {
            self.right = levels >> lr & 1 == 1;
        }
    }

    /// The receive format starts a stream, counted from its time: the
    /// receiver discards what was queued before it.
    fn on_i2s_format(&mut self, format: &I2sFormat, at_ns: u64) {
        self.at(at_ns);
        if format.receive {
            self.frame = audio::Frame::of(format);
            self.start_ns = at_ns;
            self.sent = 0;
        }
    }

    /// The samples from where the last ones ended to `LEAD_NS` past now, as
    /// many as fit one message.
    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        self.at(now_ns);
        let frame = self.frame?;
        // Behind, after a long jump between two polls: the receiver has gone
        // on without them, and samples for a time already past add delay.
        if self.sent_until(frame) < now_ns {
            self.start_ns = now_ns;
            self.sent = 0;
        }
        let due = frame.frames_in(now_ns + LEAD_NS - self.start_ns);
        let room = ((BUFFER - ENVELOPE) / frame.bytes()) as u64;
        let frames = due.saturating_sub(self.sent).min(room);
        if frames == 0 {
            return None;
        }
        let pcm = self.tone(frame, frames as usize);
        self.sent += frames;
        Some(samples(&pcm, now_ns))
    }

    /// Somebody changed the tone. The names are the part's own, and
    /// `set:tone-hz=` and `set:level-dbfs=` put a value in one step, rounded
    /// to a whole Hz or dB.
    fn on_input(&mut self, name: &str) {
        match set_value(name) {
            Some(("tone-hz", hz)) => self.tone_hz = hz.round().clamp(MIN_HZ as f64, MAX_HZ as f64) as u32,
            Some(("level-dbfs", db)) => self.level_dbfs = db.round().clamp(MIN_DBFS as f64, 0.0) as i32,
            Some(_) => {} // Not a value this part has.
            None => match name {
                "tone" => {
                    self.fed = false;
                    self.tone_held = true;
                    self.heard.clear();
                }
                "feed" => {
                    self.fed = true;
                    self.tone_held = false;
                }
                "higher" => self.tone_hz = (self.tone_hz * 2).min(MAX_HZ),
                "lower" => self.tone_hz = (self.tone_hz / 2).max(MIN_HZ),
                "louder" => self.level_dbfs = (self.level_dbfs + STEP_DB).min(0),
                "quieter" => self.level_dbfs = (self.level_dbfs - STEP_DB).max(MIN_DBFS),
                _ => {} // Not an interaction this part has.
            },
        }
    }

    /// What its host feeds it: from now on it hears that rather than its
    /// tone, unless a person chose the tone. Taken only while it knows the
    /// receive format, the rate the samples are at.
    fn audio_in(&mut self, pcm: &[u8]) {
        let Some(frame) = self.frame else { return };
        if self.tone_held {
            return;
        }
        self.fed = true;
        self.heard.extend(pcm.chunks_exact(2).map(|b| i16::from_le_bytes([b[0], b[1]])));
        let most = (u64::from(frame.rate_hz) * HELD_MS / 1_000) as usize;
        while self.heard.len() > most {
            self.heard.pop_front();
        }
    }

    /// It hears at the receive format's rate, and makes no sound.
    fn audio_rate(&self, receive: bool) -> u32 {
        match (receive, self.frame) {
            (true, Some(f)) => f.rate_hz,
            _ => 0,
        }
    }

    fn state(&self) -> Option<State> {
        Some(
            State::new()
                .set("source", if self.fed { "fed" } else { "tone" })
                .set("tone-hz", self.tone_hz)
                .set("level-dbfs", self.level_dbfs)
                .set("slot", if self.right { "right" } else { "left" })
                .set("rate-hz", self.frame.map_or(0, |f| f.rate_hz)),
        )
    }
}

// It is sent formats only, and what it supplies is at most the buffer less
// the message's own fields. It hears a sound.
esp_part::export_part!(Inmp441, buffer = 16384, hears);

// The device runs on the host too, so `cargo test` checks it without an
// emulator.
#[cfg(test)]
mod tests {
    use super::*;
    use esp_part::prost::Message;

    /// Wired as README.md's board wires it: SD on GPIO 7, L/R on GPIO 8.
    fn wired() -> Inmp441 {
        let mut mic = Inmp441::default();
        let pins = [(SD.to_string(), 7), (LR.to_string(), 8)].into();
        mic.configure(&PartConfig { pins, ..Default::default() });
        mic
    }

    /// The receive format the `i2s_echo` firmware programs: 16 kHz, two
    /// 32-bit slots.
    fn told(mic: &mut Inmp441, at_ns: u64) {
        let f = I2sFormat { receive: true, sample_rate_hz: 16_000, channels: 2, bits: 32 };
        mic.on_i2s_format(&f, at_ns);
    }

    fn pcm(msg: Option<EmulatorMessage>) -> Vec<u8> {
        match msg.and_then(|m| m.payload) {
            Some(esp_part::Payload::I2sIn(i)) => i.pcm,
            None => Vec::new(),
            Some(other) => panic!("not an I2sIn: {other:?}"),
        }
    }

    /// Each frame's left and right slots.
    fn slots(pcm: &[u8]) -> Vec<(i32, i32)> {
        let f = audio::Frame { rate_hz: 16_000, slots: 2, width: 4 };
        pcm.chunks_exact(8).map(|fr| (f.read(fr, 0), f.read(fr, 1))).collect()
    }

    #[test]
    fn its_claim_names_its_data_output_as_the_chips_input() {
        let want = I2sClaim { din_pin: Some(7), ..Default::default() };
        assert_eq!(wired().claims(), [Claim { what: Some(claim::What::I2s(want)) }]);
        let none = I2sClaim::default();
        assert_eq!(Inmp441::default().claims(), [Claim { what: Some(claim::What::I2s(none)) }]);
    }

    #[test]
    fn it_says_nothing_before_the_receive_format() {
        let mut mic = wired();
        assert!(mic.poll(1_000).is_none());
        let transmit = I2sFormat { receive: false, sample_rate_hz: 16_000, channels: 2, bits: 32 };
        mic.on_i2s_format(&transmit, 2_000);
        assert!(mic.poll(3_000).is_none(), "the transmit format is not its business");
    }

    #[test]
    fn it_keeps_the_lead_queued_and_then_what_time_has_used() {
        let mut mic = wired();
        told(&mut mic, 1_000_000);
        // 50 ms at 16 kHz is 800 frames of eight bytes.
        assert_eq!(pcm(mic.poll(1_000_000)).len(), 800 * 8);
        assert!(mic.poll(1_000_000).is_none(), "nothing more is due");
        // A millisecond later, a millisecond more.
        assert_eq!(pcm(mic.poll(2_000_000)).len(), 16 * 8);
    }

    #[test]
    fn behind_it_starts_again_from_now_rather_than_catch_up() {
        let mut mic = wired();
        told(&mut mic, 0);
        pcm(mic.poll(0));
        // A second without a poll: the lead from now, not a second of backlog.
        assert_eq!(pcm(mic.poll(1_000_000_000)).len(), 800 * 8);
    }

    #[test]
    fn a_chip_reset_stops_it_until_the_next_format() {
        let mut mic = wired();
        told(&mut mic, 5_000_000);
        pcm(mic.poll(5_000_000));
        mic.on_pins(u64::MAX, 100);
        assert!(mic.poll(200).is_none(), "time went back: the receiver is gone");
        told(&mut mic, 300);
        assert_eq!(pcm(mic.poll(300)).len(), 800 * 8);
    }

    #[test]
    fn l_r_picks_the_half_and_the_other_is_zeros() {
        let mut mic = wired();
        told(&mut mic, 0);
        let left = slots(&pcm(mic.poll(0)));
        assert!(left.iter().all(|&(_, r)| r == 0), "right undriven");
        assert!(left.iter().any(|&(l, _)| l != 0));
        assert!(left.iter().all(|&(l, _)| l & 0xFF == 0), "24 bits in a 32-bit slot");

        let mut mic = wired();
        mic.on_pins(1 << 8, 0);
        told(&mut mic, 0);
        let right = slots(&pcm(mic.poll(0)));
        assert!(right.iter().all(|&(l, _)| l == 0) && right.iter().any(|&(_, r)| r != 0));
    }

    /// Upward zero crossings a second, in the left slot.
    fn pitch(pcm: &[u8]) -> usize {
        let left: Vec<i32> = slots(pcm).into_iter().map(|(l, _)| l).collect();
        let crossings = left.windows(2).filter(|w| w[0] < 0 && w[1] >= 0).count();
        crossings * 16_000 / left.len()
    }

    #[test]
    fn the_tone_is_the_pitch_and_level_it_was_set_to() {
        let mut mic = wired();
        told(&mut mic, 0);
        let mut heard = Vec::new();
        // A second of samples, a millisecond a poll.
        for ms in 0..1_000u64 {
            heard.extend(pcm(mic.poll(ms * 1_000_000)));
        }
        assert!((pitch(&heard) as i64 - 440).abs() <= 2, "{} Hz", pitch(&heard));
        let peak = slots(&heard).iter().map(|&(l, _)| l.unsigned_abs()).max().unwrap();
        let db = 20.0 * (f64::from(peak) / f64::from(i32::MAX)).log10();
        assert!((db + 12.0).abs() < 0.1, "{db} dBFS");

        mic.on_input("higher");
        assert_eq!(mic.state().unwrap().get("tone-hz"), Some("880"));
        let next = pcm(mic.poll(2_000_000_000));
        assert!((pitch(&next) as i64 - 880).abs() <= 25, "{} Hz", pitch(&next));
    }

    #[test]
    fn a_tone_above_half_the_rate_is_silence() {
        let mut mic = wired();
        mic.on_input("set:tone-hz=9000");
        told(&mut mic, 0);
        assert!(pcm(mic.poll(0)).iter().all(|&b| b == 0));
    }

    #[test]
    fn a_fast_wide_stream_is_sent_a_buffer_at_a_time() {
        // 48 kHz, eight 32-bit slots: 50 ms is 76,800 bytes, more than one
        // message holds.
        let mut mic = wired();
        let f = I2sFormat { receive: true, sample_rate_hz: 48_000, channels: 8, bits: 32 };
        mic.on_i2s_format(&f, 0);
        let first = mic.poll(0).unwrap();
        assert_eq!(pcm(Some(first.clone())).len() % 32, 0, "whole frames");
        assert!(EmulatorMessage { sim_time_ns: u64::MAX, ..first }.encoded_len() <= BUFFER);
        assert!(mic.poll(0).is_some(), "the rest of the lead follows");
    }

    /// `samples` as the 16-bit little-endian bytes a host feeds.
    fn bytes(samples: &[i16]) -> Vec<u8> {
        samples.iter().flat_map(|s| s.to_le_bytes()).collect()
    }

    #[test]
    fn what_it_is_fed_it_supplies_in_its_slot_then_silence() {
        let mut mic = wired();
        assert_eq!(mic.audio_rate(true), 0, "no format yet");
        told(&mut mic, 0);
        assert_eq!((mic.audio_rate(true), mic.audio_rate(false)), (16_000, 0));
        mic.audio_in(&bytes(&[1_000, -1_000, 32_767]));
        assert_eq!(mic.state().unwrap().get("source"), Some("fed"));
        let got = slots(&pcm(mic.poll(0)));
        assert_eq!(got.len(), 800, "the lead, as for the tone");
        assert_eq!(&got[..3], &[(1_000 << 16, 0), (-1_000 << 16, 0), (32_767 << 16, 0)]);
        assert!(got[3..].iter().all(|&s| s == (0, 0)), "the feed ran dry");
    }

    #[test]
    fn tone_holds_the_tone_while_it_is_fed_until_feed() {
        let mut mic = wired();
        told(&mut mic, 0);
        mic.audio_in(&bytes(&[5; 100]));
        mic.on_input("tone");
        // A host that goes on feeding does not take it back.
        mic.audio_in(&bytes(&[5; 100]));
        assert_eq!(mic.state().unwrap().get("source"), Some("tone"));
        let got = slots(&pcm(mic.poll(0)));
        assert!(got.iter().any(|&(l, _)| l != 0) && got.iter().all(|&(l, _)| l != 5 << 16), "the tone again");
        mic.on_input("feed");
        mic.audio_in(&bytes(&[5; 100]));
        assert_eq!(mic.state().unwrap().get("source"), Some("fed"));
        assert_eq!(slots(&pcm(mic.poll(1_000_000)))[0].0, 5 << 16);
    }

    #[test]
    fn it_keeps_a_quarter_second_of_what_it_is_fed_and_none_before_a_format() {
        let mut mic = wired();
        mic.audio_in(&bytes(&[7; 10]));
        assert_eq!(mic.state().unwrap().get("source"), Some("tone"), "fed before it knew the rate");
        told(&mut mic, 1_000);
        // A second at 16 kHz: the newest 4000 samples are kept.
        let second: Vec<i16> = (0..16_000).map(|n| n as i16).collect();
        mic.audio_in(&bytes(&second));
        assert_eq!(slots(&pcm(mic.poll(1_000)))[0].0, 12_000 << 16);
        // A chip reset forgets what it held: after the next format, silence.
        mic.on_pins(u64::MAX, 100);
        told(&mut mic, 200);
        assert!(slots(&pcm(mic.poll(200))).iter().all(|&s| s == (0, 0)));
    }

    #[test]
    fn the_inputs_move_the_tone_and_stop_at_the_range() {
        let mut mic = wired();
        for _ in 0..10 {
            mic.on_input("higher");
        }
        assert_eq!(mic.tone_hz, MAX_HZ);
        mic.on_input("set:tone-hz=440.4");
        mic.on_input("lower");
        assert_eq!(mic.tone_hz, 220);
        mic.on_input("set:tone-hz=1");
        assert_eq!(mic.tone_hz, MIN_HZ);
        mic.on_input("louder");
        mic.on_input("louder");
        mic.on_input("louder");
        assert_eq!(mic.level_dbfs, 0);
        mic.on_input("set:level-dbfs=-200");
        assert_eq!(mic.level_dbfs, MIN_DBFS);
        mic.on_input("wiggle");
        let shown = mic.state().unwrap();
        assert_eq!(shown.get("level-dbfs"), Some("-90"));
        assert_eq!(shown.get("slot"), Some("left"));
        assert_eq!(shown.get("rate-hz"), Some("0"), "not told a format yet");
    }
}
