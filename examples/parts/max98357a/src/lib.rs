//! A Maxim MAX98357A I2S class-D amplifier, written as a virtual part.
//!
//! A part is a WebAssembly module that models a component wired to the chip's
//! pins. This one listens on the chip's I2S data output. The amplifier takes
//! I2S with 16, 24 or 32 bits a slot and plays one channel, picked by the
//! voltage on its SD_MODE pin: below 0.16 V it is shut down, to 0.77 V it
//! plays the mix of both, (L+R)/2, to 1.4 V the right channel and above that
//! the left (the "MAX98357A/MAX98357B" datasheet, Maxim, SD_MODE table). A
//! chip pin drives SD_MODE at a logic level, so high is the left channel and
//! low shuts it down; the right channel needs a voltage between the two,
//! which no logic level is. Left unwired here, it plays the mix: a breakout
//! board holds SD_MODE in that band with its own resistors.
//! README.md lists what this leaves out.
//!
//! What reaches its speaker it hands to its host as sound (`part_audio_out`),
//! for a page to play or the command-line emulator to record, and it shows it
//! (`part_state`): whether it is playing, the pitch, from the upward zero
//! crossings, and the peak level, over the last 100 ms.

use std::collections::VecDeque;

use esp_part::prelude::*;

/// The amplifier's own names for its pins (the datasheet's pin table). The
/// board says which chip pins they are wired to, as `{ pins: { DIN: 6 } }`.
const DIN: &str = "DIN";
const SD_MODE: &str = "SD_MODE";

/// How much of what it plays its state describes: 100 ms, at the stream's
/// rate. A window with no samples for twice that is silence.
const WINDOW_NS: u64 = 100_000_000;
/// What a silent window shows as its level, in dBFS.
const SILENT_DBFS: i32 = -99;
/// How much sound it keeps for its host, in ms of the stream: sound nobody
/// collected is delay, so the oldest goes past this.
const HELD_MS: u64 = 250;

/// What reaches the speaker.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Channel {
    Left,
    Mix,
    Shutdown,
}

/// What the last whole window held.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Shown {
    playing: bool,
    hz: u32,
    level_dbfs: i32,
}

#[derive(Default)]
struct Max98357a {
    /// The chip pins DIN and SD_MODE are wired to. `None` until the board
    /// says, or when it left the pin unconnected.
    din: Option<u32>,
    sd_mode: Option<u32>,
    /// SD_MODE's level, while a chip pin drives it.
    sd_high: bool,
    /// The transmit format, once the chip has said it.
    frame: Option<audio::Frame>,
    /// The bytes of a frame a message ended inside.
    rest: Vec<u8>,
    /// The window being filled: samples in it, the last one, its peak, and
    /// where its upward zero crossings fell, in samples.
    count: u32,
    last: i32,
    peak: u32,
    first_crossing: Option<f64>,
    last_crossing: f64,
    crossings: u32,
    /// The last whole window, and when samples last arrived.
    shown: Shown,
    heard_ns: u64,
    /// The latest time it was told: an earlier one means the chip reset.
    last_ns: u64,
    /// What reached the speaker and its host has not taken yet, as 16-bit
    /// samples.
    held: VecDeque<i16>,
}

impl Max98357a {
    fn channel(&self) -> Channel {
        match self.sd_mode {
            None => Channel::Mix,
            Some(_) if self.sd_high => Channel::Left,
            Some(_) => Channel::Shutdown,
        }
    }

    /// Note the time. One earlier than the last means the chip reset, and
    /// what was playing stopped with it.
    fn at(&mut self, now_ns: u64) {
        if now_ns < self.last_ns {
            self.quiet();
            // The stream it was playing went with the chip; the next comes
            // after a format.
            self.frame = None;
            self.held.clear();
        }
        self.last_ns = now_ns;
    }

    /// Nothing is playing: the window starts again empty.
    fn quiet(&mut self) {
        self.shown = Shown { playing: false, hz: 0, level_dbfs: SILENT_DBFS };
        self.rest.clear();
        self.restart();
    }

    fn restart(&mut self) {
        self.count = 0;
        self.peak = 0;
        self.first_crossing = None;
        self.crossings = 0;
    }

    /// One sample reaching the speaker.
    fn play(&mut self, sample: i32, rate_hz: u32) {
        self.held.push_back((sample >> 16) as i16);
        let most = (u64::from(rate_hz) * HELD_MS / 1_000) as usize;
        while self.held.len() > most {
            self.held.pop_front();
        }
        if self.last < 0 && sample >= 0 {
            // Where between the two samples the wave crossed zero.
            let at = f64::from(self.count) - f64::from(sample) / (f64::from(sample) - f64::from(self.last));
            self.first_crossing.get_or_insert(at);
            self.last_crossing = at;
            self.crossings += 1;
        }
        self.last = sample;
        self.peak = self.peak.max(sample.unsigned_abs());
        self.count += 1;
        if u64::from(self.count) * 1_000_000_000 >= WINDOW_NS * u64::from(rate_hz) {
            self.shown = self.measure(rate_hz);
            self.restart();
        }
    }

    /// The pitch and level of the window just filled.
    fn measure(&self, rate_hz: u32) -> Shown {
        let hz = match self.first_crossing {
            Some(first) if self.crossings >= 2 && self.last_crossing > first => {
                f64::from(self.crossings - 1) * f64::from(rate_hz) / (self.last_crossing - first)
            }
            _ => 0.0,
        };
        let level = if self.peak == 0 {
            SILENT_DBFS
        } else {
            (20.0 * (f64::from(self.peak) / f64::from(i32::MAX)).log10()).round().max(f64::from(SILENT_DBFS)) as i32
        };
        Shown { playing: self.peak != 0, hz: hz.round() as u32, level_dbfs: level }
    }
}

impl Device for Max98357a {
    const NAME: &'static str = "max98357a";
    const ON_MESSAGE: bool = true;

    /// Where the board wired the part.
    fn configure(&mut self, config: &PartConfig) {
        self.din = config.pins.get(DIN).copied();
        self.sd_mode = config.pins.get(SD_MODE).copied();
        self.quiet();
    }

    /// Its data input, DIN, is the chip's data output. An amplifier the
    /// board left without DIN still claims, with no pad, so the emulator
    /// refuses the claim and says so.
    fn claims(&self) -> Vec<Claim> {
        vec![i2s(self.din, None)]
    }

    fn on_pins(&mut self, levels: u64, now_ns: u64) {
        self.at(now_ns);
        if let Some(pin) = self.sd_mode {
            self.sd_high = levels >> pin & 1 == 1;
        }
    }

    /// The transmit format: the layout of what it is about to hear.
    fn on_i2s_format(&mut self, format: &I2sFormat, at_ns: u64) {
        self.at(at_ns);
        if !format.receive {
            let frame = audio::Frame::of(format);
            if frame.map(|f| f.rate_hz) != self.frame.map(|f| f.rate_hz) {
                // Samples at the old rate would play at the wrong pitch.
                self.held.clear();
            }
            self.frame = frame;
            self.rest.clear();
            self.restart();
        }
    }

    /// What the chip clocked out: one channel of it, or the mix, reaches the
    /// speaker, unless SD_MODE has it shut down.
    fn on_i2s_out(&mut self, pcm: &[u8], at_ns: u64) {
        self.at(at_ns);
        let Some(frame) = self.frame else { return };
        let channel = self.channel();
        if channel == Channel::Shutdown {
            self.quiet();
            return;
        }
        self.heard_ns = at_ns;
        let mut bytes = std::mem::take(&mut self.rest);
        bytes.extend_from_slice(pcm);
        let whole = bytes.len() - bytes.len() % frame.bytes();
        let right = if frame.slots > 1 { 1 } else { 0 };
        for f in bytes[..whole].chunks_exact(frame.bytes()) {
            let sample = match channel {
                Channel::Left => frame.read(f, 0),
                _ => ((i64::from(frame.read(f, 0)) + i64::from(frame.read(f, right))) / 2) as i32,
            };
            self.play(sample, frame.rate_hz);
        }
        self.rest = bytes.split_off(whole);
    }

    /// It never speaks; a poll is only the time passing, and samples that
    /// stopped coming are silence.
    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        self.at(now_ns);
        if self.shown.playing && now_ns.saturating_sub(self.heard_ns) > 2 * WINDOW_NS {
            self.quiet();
        }
        None
    }

    /// What reached the speaker since its host last asked, oldest first, as
    /// much as `out` holds; the rest waits for the next call.
    fn audio_out(&mut self, out: &mut [u8]) -> usize {
        let n = self.held.len().min(out.len() / 2);
        for (pair, sample) in out.chunks_exact_mut(2).zip(self.held.drain(..n)) {
            pair.copy_from_slice(&sample.to_le_bytes());
        }
        2 * n
    }

    /// It makes its sound at the transmit format's rate, and hears nothing.
    fn audio_rate(&self, receive: bool) -> u32 {
        match (receive, self.frame) {
            (false, Some(f)) => f.rate_hz,
            _ => 0,
        }
    }

    fn state(&self) -> Option<State> {
        let channel = match self.channel() {
            Channel::Left => "left",
            Channel::Mix => "mix",
            Channel::Shutdown => "shutdown",
        };
        Some(
            State::new()
                .set("playing", self.shown.playing)
                .set("frequency-hz", self.shown.hz)
                .set("level-dbfs", self.shown.level_dbfs)
                .set("channel", channel),
        )
    }
}

// A host hands over what the chip transmits 4096 bytes at a time, in a
// message of up to 4114 bytes. It makes a sound.
esp_part::export_part!(Max98357a, buffer = 8192, sounds);

// The device runs on the host too, so `cargo test` checks it without an
// emulator.
#[cfg(test)]
mod tests {
    use super::*;
    use std::f64::consts::TAU;

    /// Wired as README.md's board wires it: DIN on GPIO 6, SD_MODE left
    /// unwired unless `sd_mode` says.
    fn amp(sd_mode: Option<u32>) -> Max98357a {
        let mut amp = Max98357a::default();
        let mut pins: std::collections::BTreeMap<String, u32> = [(DIN.to_string(), 6)].into();
        if let Some(pin) = sd_mode {
            pins.insert(SD_MODE.to_string(), pin);
        }
        amp.configure(&PartConfig { pins: pins.into_iter().collect(), ..Default::default() });
        amp
    }

    /// `ms` milliseconds of a `hz` tone at `dbfs` in the left slot of
    /// `bits`-bit stereo at 16 kHz, the right slot silent.
    fn tone(hz: f64, dbfs: f64, ms: u32, bits: u32) -> (I2sFormat, Vec<u8>) {
        let format = I2sFormat { receive: false, sample_rate_hz: 16_000, channels: 2, bits };
        let f = audio::Frame::of(&format).unwrap();
        let peak = f64::from(i32::MAX) * 10f64.powf(dbfs / 20.0);
        let mut pcm = vec![0; 16 * ms as usize * f.bytes()];
        for (n, fr) in pcm.chunks_exact_mut(f.bytes()).enumerate() {
            f.write(fr, 0, (peak * (TAU * hz * n as f64 / 16_000.0).sin()) as i32);
        }
        (format, pcm)
    }

    fn shown(amp: &Max98357a) -> (String, String, String, String) {
        let s = amp.state().unwrap();
        let get = |k| s.get(k).unwrap().to_string();
        (get("playing"), get("frequency-hz"), get("level-dbfs"), get("channel"))
    }

    #[test]
    fn its_claim_names_its_data_input_as_the_chips_output() {
        let want = I2sClaim { dout_pin: Some(6), ..Default::default() };
        assert_eq!(amp(None).claims(), [Claim { what: Some(claim::What::I2s(want)) }]);
    }

    #[test]
    fn unwired_it_plays_the_mix_and_shows_its_pitch_and_level() {
        let mut amp = amp(None);
        assert_eq!(shown(&amp).0, "false", "silent before anything is sent");
        let (format, pcm) = tone(1_000.0, -6.0, 300, 32);
        amp.on_i2s_format(&format, 0);
        // As the emulator sends it: at most 4096 bytes a message.
        for piece in pcm.chunks(4_096) {
            amp.on_i2s_out(piece, 1_000);
        }
        // (L+R)/2 with the right silent is half the left: 6 dB down.
        assert_eq!(shown(&amp), ("true".into(), "1000".into(), "-12".into(), "mix".into()));
    }

    #[test]
    fn sd_mode_high_is_the_left_channel_and_low_shuts_it_down() {
        let mut amp = amp(Some(9));
        let (format, pcm) = tone(440.0, -6.0, 200, 32);
        amp.on_i2s_format(&format, 0);
        amp.on_pins(1 << 9, 0);
        amp.on_i2s_out(&pcm, 1_000);
        assert_eq!(shown(&amp), ("true".into(), "440".into(), "-6".into(), "left".into()));
        amp.on_pins(0, 2_000);
        amp.on_i2s_out(&pcm, 3_000);
        assert_eq!(shown(&amp), ("false".into(), "0".into(), "-99".into(), "shutdown".into()));
    }

    #[test]
    fn sixteen_and_twenty_four_bit_slots_play_alike() {
        for bits in [16, 24] {
            let mut amp = amp(None);
            let (format, pcm) = tone(523.0, -20.0, 200, bits);
            amp.on_i2s_format(&format, 0);
            amp.on_i2s_out(&pcm, 1_000);
            assert_eq!(shown(&amp).1, "523", "{bits}-bit");
            assert_eq!(shown(&amp).2, "-26", "{bits}-bit");
        }
    }

    #[test]
    fn a_frame_split_between_two_messages_is_joined() {
        let mut amp = amp(None);
        let (format, pcm) = tone(1_000.0, -6.0, 200, 24);
        amp.on_i2s_format(&format, 0);
        // Six-byte frames, cut one byte into a frame.
        let (a, b) = pcm.split_at(6 * 700 + 1);
        amp.on_i2s_out(a, 1_000);
        amp.on_i2s_out(b, 2_000);
        assert_eq!(shown(&amp).1, "1000");
    }

    /// Every sample its host takes, in order.
    fn heard(amp: &mut Max98357a) -> Vec<i16> {
        let mut out = vec![0; 8192];
        let mut all = Vec::new();
        loop {
            let n = amp.audio_out(&mut out);
            if n == 0 {
                return all;
            }
            all.extend(out[..n].chunks_exact(2).map(|b| i16::from_le_bytes([b[0], b[1]])));
        }
    }

    #[test]
    fn what_reaches_the_speaker_is_its_sound_the_last_quarter_second_of_it() {
        let mut amp = amp(None);
        assert_eq!(amp.audio_rate(false), 0, "no format yet");
        let (format, pcm) = tone(1_000.0, -6.0, 300, 32);
        amp.on_i2s_format(&format, 0);
        for piece in pcm.chunks(4_096) {
            amp.on_i2s_out(piece, 1_000);
        }
        assert_eq!((amp.audio_rate(false), amp.audio_rate(true)), (16_000, 0));
        let sound = heard(&mut amp);
        // 250 ms of 16 kHz: the first 50 ms went for the newest.
        assert_eq!(sound.len(), 4_000);
        let f = audio::Frame::of(&format).unwrap();
        let mix = |n: usize| {
            let fr = &pcm[n * f.bytes()..(n + 1) * f.bytes()];
            (((i64::from(f.read(fr, 0)) + i64::from(f.read(fr, 1))) / 2) as i32 >> 16) as i16
        };
        assert_eq!(sound[0], mix(800));
        assert_eq!(sound[3_999], mix(4_799));
        assert!(heard(&mut amp).is_empty(), "taken once");
    }

    #[test]
    fn shut_down_it_makes_no_sound_and_a_reset_forgets_the_stream() {
        let mut amp = amp(Some(9));
        let (format, pcm) = tone(440.0, -6.0, 100, 16);
        amp.on_i2s_format(&format, 1_000);
        amp.on_pins(0, 1_000);
        amp.on_i2s_out(&pcm, 2_000);
        assert!(heard(&mut amp).is_empty(), "SD_MODE low");
        amp.on_pins(1 << 9, 3_000);
        amp.on_i2s_out(&pcm, 4_000);
        assert_eq!(heard(&mut amp).len(), 1_600);
        amp.on_i2s_out(&pcm, 5_000);
        amp.on_pins(1 << 9, 10);
        assert!(heard(&mut amp).is_empty() && amp.audio_rate(false) == 0, "time went back: the chip reset");
    }

    #[test]
    fn samples_that_stop_coming_are_silence() {
        let mut amp = amp(None);
        let (format, pcm) = tone(1_000.0, -6.0, 200, 32);
        amp.on_i2s_format(&format, 0);
        amp.on_i2s_out(&pcm, 200_000_000);
        assert!(amp.poll(300_000_000).is_none());
        assert_eq!(shown(&amp).0, "true");
        amp.poll(500_000_000);
        assert_eq!(shown(&amp).0, "false", "nothing for 300 ms");
    }
}
