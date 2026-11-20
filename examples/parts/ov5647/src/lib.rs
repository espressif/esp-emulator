//! An OmniVision OV5647 camera sensor on the ESP32-P4's MIPI CSI link, in a
//! colour-bar test mode, written as a virtual part.
//!
//! The firmware talks to it the way `esp_cam_sensor`'s OV5647 driver does:
//! over SCCB (I2C at 0x36, 16-bit register addresses), reading its chip ID
//! and programming its output: the RAW bit depth (`0x3034`), the output size
//! (`0x3808` to `0x380B`) and streaming (`0x0100`). While it streams and the
//! CSI host captures on two lanes, the part sends a GBRG Bayer mosaic at that
//! size, a frame a period: the last picture it was shown in that mode, else
//! the sensor's eight colour bars. README.md lists what it leaves out.
//!
//! `Ov5647` is the device and knows nothing about WebAssembly; the
//! `export_part!` line at the end makes it a part.

use std::collections::BTreeMap;

use esp_part::camera::{fourcc, Mode, Sensor, MAX_FPS};
use esp_part::prelude::*;

/// The sensor's address on SCCB.
const DEFAULT_ADDR: u32 = 0x36;

/// The chip ID, `0x5647`.
const CHIP_ID: [(u16, u8); 2] = [(0x300A, 0x56), (0x300B, 0x47)];
/// Bit 0: streaming, else asleep.
const MODE_SELECT: u16 = 0x0100;
/// Bit 0: every register back to its power-on value.
const SOFTWARE_RESET: u16 = 0x0103;
/// The MIPI bit mode in its low nibble: 8 for RAW8, 10 for RAW10.
const MIPI_BIT_MODE: u16 = 0x3034;
/// The output size: bits 11:8 of the width, its low byte, then the height's.
const X_OUTPUT_SIZE: u16 = 0x3808;
const Y_OUTPUT_SIZE: u16 = 0x380A;
/// The line and frame lengths in pixel clocks and lines (HTS, VTS), high
/// byte first.
const TIMING_HTS: u16 = 0x380C;
const TIMING_VTS: u16 = 0x380E;
/// The PLL multiplier. `esp_cam_sensor` writes it as the pixel clock times
/// 32 for RAW8, or 30 for RAW10, over 25 MHz.
const PLL_MULTIPLIER: u16 = 0x3036;

/// The data lanes the module is wired for.
const LANES: u32 = 2;

/// The frame rate before the firmware programs the timing.
const DEFAULT_FPS: f64 = 15.0;

#[derive(Default)]
struct Ov5647 {
    sioc: Option<u32>,
    siod: Option<u32>,
    addr: u32,
    /// The registers as written; a register never written reads 0.
    regs: BTreeMap<u16, u8>,
    /// The register an SCCB read starts at.
    pointer: u16,
    /// The CSI host's lanes, and whether it captures, as last told.
    host: Option<(bool, u32, u32)>,
    /// Its frames, rate and bars.
    camera: Sensor,
}

impl Ov5647 {
    fn reg(&self, reg: u16) -> u8 {
        CHIP_ID.iter().find(|(r, _)| *r == reg).map_or_else(|| self.regs.get(&reg).copied().unwrap_or(0), |(_, v)| *v)
    }

    /// The rate the firmware programmed: the pixel clock over a frame's
    /// pixel clocks, HTS × VTS; [`DEFAULT_FPS`] until all three are set.
    fn native_fps(&self) -> f64 {
        let word = |at: u16, high_mask: u8| u32::from(self.reg(at) & high_mask) << 8 | u32::from(self.reg(at + 1));
        let (hts, vts) = (word(TIMING_HTS, 0x1F), word(TIMING_VTS, 0xFF));
        let per_clock = match self.reg(MIPI_BIT_MODE) & 0x0F {
            0x08 => 32.0,
            0x0A => 30.0,
            _ => 0.0,
        };
        let multiplier = f64::from(self.reg(PLL_MULTIPLIER));
        if hts == 0 || vts == 0 || multiplier == 0.0 || per_clock == 0.0 {
            return DEFAULT_FPS;
        }
        let pixel_clock = multiplier * 25e6 / per_clock;
        (pixel_clock / (f64::from(hts) * f64::from(vts))).clamp(1.0, MAX_FPS)
    }

    fn streaming(&self) -> bool {
        self.reg(MODE_SELECT) & 0x01 != 0
    }

    /// The output the firmware programmed: `None` until it has set a size
    /// and a RAW bit depth.
    fn mode(&self) -> Option<Mode> {
        let size = |at: u16| u32::from(self.reg(at) & 0x0F) << 8 | u32::from(self.reg(at + 1));
        let (width, height) = (size(X_OUTPUT_SIZE), size(Y_OUTPUT_SIZE));
        let name = match self.reg(MIPI_BIT_MODE) & 0x0F {
            0x08 => b"GBRG",
            0x0A => b"GB10",
            _ => return None,
        };
        (width > 0 && height > 0).then(|| Mode { width, height, fourcc: fourcc(name) })
    }

    fn write(&mut self, reg: u16, value: u8) {
        if reg == SOFTWARE_RESET && value & 0x01 != 0 {
            self.regs.clear();
            self.retell();
            return;
        }
        self.regs.insert(reg, value);
        if reg == MODE_SELECT {
            self.retell();
        }
    }

    /// Tell the stream again whether frames go out: the host must capture on
    /// the module's lanes and the sensor must stream.
    fn retell(&mut self) {
        if let Some((capturing, lanes, session)) = self.host {
            let on = capturing && lanes == LANES && self.streaming();
            let (now, fps) = (self.camera.now(), self.native_fps());
            self.camera.format(on, session, now, fps);
        }
    }
}

impl Device for Ov5647 {
    const NAME: &'static str = "ov5647";
    const ON_BUS: bool = true;
    const ON_MESSAGE: bool = true;

    fn configure(&mut self, config: &PartConfig) {
        self.sioc = config.pins.get("SIOC").copied();
        self.siod = config.pins.get("SIOD").copied();
        self.addr = if config.addr == 0 { DEFAULT_ADDR } else { config.addr };
    }

    /// SCCB's address on the pins wired to it, and the CSI host: its lanes
    /// are dedicated pads, so a board wires only SCCB. Unwired, it claims
    /// nothing.
    fn claims(&self) -> Vec<Claim> {
        if self.sioc.is_none() && self.siod.is_none() {
            return Vec::new();
        }
        let i2c = I2cClaim { addr: self.addr, scl_pin: self.sioc, sda_pin: self.siod };
        vec![Claim { what: Some(claim::What::I2c(i2c)) }, csi()]
    }

    /// An SCCB transaction: the first two bytes written are the register,
    /// most significant first, the rest are stored from it on; a read
    /// answers from the register last named.
    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        let mut reply = BusReply { correlation_id: request.correlation_id, op: BusOp::I2c as i32, ..Default::default() };
        let Some(bus_request::Op::I2c(i2c)) = &request.op else { return Some(reply) };
        if i2c.addr != self.addr {
            return Some(reply);
        }
        if let [hi, lo, values @ ..] = i2c.write.as_slice() {
            self.pointer = u16::from_be_bytes([*hi, *lo]);
            for &v in values {
                self.write(self.pointer, v);
                self.pointer = self.pointer.wrapping_add(1);
            }
        }
        reply.data = (0..i2c.read_len)
            .map(|_| {
                let v = self.reg(self.pointer);
                self.pointer = self.pointer.wrapping_add(1);
                v
            })
            .collect();
        reply.ack = true;
        Some(reply)
    }

    fn on_csi_format(&mut self, format: &CsiFormat, at_ns: u64) {
        self.host = Some((format.capturing, format.lanes, format.session));
        let on = format.capturing && format.lanes == LANES && self.streaming();
        let fps = self.native_fps();
        self.camera.format(on, format.session, at_ns, fps);
    }

    /// The bars in the mode programmed now, a frame a period.
    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        let (mode, fps) = (self.mode(), self.native_fps());
        self.camera.poll(now_ns, fps, PART_BUFFER, mode)
    }

    /// What to show it: an RGB888 picture of the mode programmed now, at its
    /// rate; nothing larger than its buffer takes.
    fn video_format(&mut self) -> Option<VideoFormat> {
        let (mode, fps) = (self.mode(), self.native_fps());
        self.camera.video_format(mode, fps, PART_BUFFER)
    }

    /// A picture for the mode programmed now, made into its mosaic, kept and
    /// sent as its frames.
    fn video_in(&mut self, picture: &[u8]) -> Result<(), String> {
        let mode = self.mode();
        self.camera.video_in(mode, picture)
    }

    /// `set:fps=<n>`: the frame rate, from 1 to 60 a second, over the one
    /// the registers give.
    fn on_input(&mut self, name: &str) {
        self.camera.on_input(name);
    }

    fn state(&self) -> Option<State> {
        Some(self.camera.state(self.mode(), self.native_fps()).set("streaming", self.streaming()))
    }
}

// A picture it is shown comes in whole: RGB888 up to its largest mode,
// 1920x1080 (6.2 MB).
esp_part::export_part!(Ov5647, buffer = 6220800, sees);

#[cfg(test)]
mod tests {
    use super::*;
    use esp_part::Payload;

    fn wired() -> Ov5647 {
        let mut sensor = Ov5647::default();
        let pins = [("SIOC".to_string(), 8), ("SIOD".to_string(), 7)];
        sensor.configure(&PartConfig { pins: pins.into_iter().collect(), ..Default::default() });
        sensor
    }

    fn sccb(sensor: &mut Ov5647, write: &[u8], read_len: u32) -> Vec<u8> {
        let i2c = I2cTransaction { bus: 0, addr: DEFAULT_ADDR, write: write.to_vec(), read_len, continues: false };
        let reply = sensor.on_bus(&BusRequest { correlation_id: 3, op: Some(bus_request::Op::I2c(i2c)) }).unwrap();
        assert!(reply.ack && reply.correlation_id == 3);
        reply.data
    }

    /// 320 x 240 RAW8, as `test_apps/camera_part` programs it.
    fn program_qvga_raw8(sensor: &mut Ov5647) {
        for w in [[0x30, 0x34, 0x18], [0x38, 0x08, 0x01], [0x38, 0x09, 0x40], [0x38, 0x0A, 0x00], [0x38, 0x0B, 0xF0]] {
            sccb(sensor, &w, 0);
        }
    }

    #[test]
    fn it_claims_sccb_and_the_csi_host_once_wired() {
        let claims = wired().claims();
        assert!(matches!(&claims[0].what, Some(claim::What::I2c(c)) if c.addr == 0x36 && c.scl_pin == Some(8)));
        assert!(matches!(&claims[1].what, Some(claim::What::Csi(_))));
        assert!(Ov5647::default().claims().is_empty(), "unwired, it claims nothing");
    }

    #[test]
    fn the_chip_id_and_the_output_programmed_read_back() {
        let mut sensor = wired();
        assert_eq!(sccb(&mut sensor, &[0x30, 0x0A], 2), [0x56, 0x47]);
        program_qvga_raw8(&mut sensor);
        assert_eq!(sensor.mode(), Some(Mode { width: 320, height: 240, fourcc: fourcc(b"GBRG") }));
        assert_eq!(sccb(&mut sensor, &[0x38, 0x08], 4), [0x01, 0x40, 0x00, 0xF0]);
        sccb(&mut sensor, &[0x30, 0x34, 0x1A], 0);
        assert_eq!(sensor.mode().map(|m| m.fourcc), Some(fourcc(b"GB10")));
        sccb(&mut sensor, &[0x01, 0x03, 0x01], 0);
        assert_eq!(sensor.mode(), None, "a software reset forgets it");
    }

    #[test]
    fn frames_go_out_while_it_streams_and_the_host_captures_on_its_lanes() {
        let mut sensor = wired();
        program_qvga_raw8(&mut sensor);
        let capture = |lanes| CsiFormat { capturing: true, lanes, session: 1 };
        sensor.on_csi_format(&capture(2), 1_000);
        assert!(sensor.poll(1_000).is_none(), "asleep until 0x0100 says stream");
        sccb(&mut sensor, &[0x01, 0x00, 0x01], 0);
        let mut frame = Vec::<u8>::new();
        let start = loop {
            match sensor.poll(1_000) {
                Some(EmulatorMessage { sim_time_ns, payload: Some(Payload::CamFrame(f)) }) => {
                    frame.extend(&f.data);
                    if f.end_of_frame {
                        assert_eq!((f.width, f.height, f.fourcc, f.session), (320, 240, fourcc(b"GBRG"), 1));
                        break sim_time_ns;
                    }
                }
                other => panic!("{other:?}"),
            }
        };
        assert_eq!(start, 1_000 + 66_666_666);
        assert_eq!(frame.len(), 320 * 240);
        // Row 0 is G B G B: the second bar, yellow, has green 255, blue 0.
        assert_eq!(&frame[60..62], &[255, 0]);
        // A host on one lane decodes nothing: the sensor holds its frames.
        sensor.on_csi_format(&capture(1), 2_000);
        assert!(sensor.poll(100_000_000).is_none());
        assert_eq!(sensor.state().unwrap().get("frames"), Some("1"));
    }

    #[test]
    fn the_rate_is_the_one_the_driver_programs() {
        // `esp_cam_sensor`'s mode tables: the bit mode, the PLL multiplier,
        // HTS and VTS.
        for (bits, multiplier, hts, vts, fps) in [
            (0x1A, 0x62, 2271, 1199, 30.0), // RAW10 1920x1080, "30fps"
            (0x1A, 0x6A, 1796, 1093, 45.0), // RAW10 1280x960, "45fps"
            (0x18, 0x80, 1896, 984, 53.6),  // RAW8 800x800: its 100 MHz IDI clock over these timings
        ] {
            let mut sensor = Ov5647::default();
            assert_eq!(sensor.native_fps(), DEFAULT_FPS, "before the timing is programmed");
            for (reg, v) in [
                (MIPI_BIT_MODE, bits),
                (PLL_MULTIPLIER, multiplier),
                (TIMING_HTS, (hts >> 8) as u8),
                (TIMING_HTS + 1, hts as u8),
                (TIMING_VTS, (vts >> 8) as u8),
                (TIMING_VTS + 1, vts as u8),
            ] {
                sensor.write(reg, v);
            }
            let got = sensor.native_fps();
            assert!((got - fps).abs() / fps < 0.01, "{got} fps, not {fps}");
            sensor.on_input("set:fps=10");
            assert_eq!(sensor.state().unwrap().get("fps"), Some("10"), "set:fps overrides it");
        }
    }

    #[test]
    fn it_sends_the_picture_it_is_shown_as_its_mosaic() {
        let mut sensor = wired();
        program_qvga_raw8(&mut sensor);
        sccb(&mut sensor, &[0x01, 0x00, 0x01], 0);
        let f = sensor.video_format().unwrap();
        assert_eq!((f.width, f.height, f.fourcc), (320, 240, fourcc(b"GBRG")));
        // A green picture: GBRG's green cells carry it, the others are 0.
        let green: Vec<u8> = [0, 0x42, 0].repeat(320 * 240);
        assert_eq!(sensor.video_in(&green), Ok(()));
        sensor.on_csi_format(&CsiFormat { capturing: true, lanes: 2, session: 1 }, 0);
        let mut frame = Vec::<u8>::new();
        while let Some(EmulatorMessage { payload: Some(Payload::CamFrame(f)), .. }) = sensor.poll(0) {
            frame.extend(&f.data);
        }
        assert_eq!(frame.len(), 320 * 240);
        assert_eq!(&frame[..2], &[0x42, 0], "G then B on the first row");
        assert_eq!(&frame[320..322], &[0, 0x42], "R then G on the second");
    }
}
