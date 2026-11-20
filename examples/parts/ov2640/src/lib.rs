//! An OmniVision OV2640 camera sensor on a DVP bus, in its colour-bar test
//! mode, written as a virtual part.
//!
//! The firmware talks to it the way `esp_cam_sensor`'s OV2640 driver does:
//! over SCCB (I2C at 0x30, 8-bit registers in two banks chosen by register
//! 0xFF), reading its chip ID and programming the DSP's output size (`ZMOW`,
//! `ZMOH`, `ZMHH`) and pixel format (`IMAGE_MODE`). While the chip's camera
//! receiver captures, the part sends a frame a period at that size and in
//! that format: the last picture it was shown in that mode (a webcam's, a
//! JPEG file's), else the sensor's eight colour bars, which no JPEG mode
//! has. README.md lists what it leaves out.
//!
//! `Ov2640` is the device and knows nothing about WebAssembly; the
//! `export_part!` line at the end makes it a part.

use esp_part::camera::{fourcc, Mode, Sensor};
use esp_part::prelude::*;

/// The sensor's address on SCCB.
const DEFAULT_ADDR: u32 = 0x30;

/// Register 0xFF chooses the bank the others are in: 0 the DSP, 1 the sensor.
const BANK_SEL: u8 = 0xFF;
const BANK_DSP: u8 = 0;
const BANK_SENSOR: u8 = 1;

/// Sensor bank: the chip ID, and COM7, whose bit 7 resets every register.
const SENSOR_ID: [(u8, u8); 4] = [(0x0A, 0x26), (0x0B, 0x42), (0x1C, 0x7F), (0x1D, 0xA2)];
const COM7: u8 = 0x12;
const COM7_SRST: u8 = 0x80;

/// DSP bank: the output size in units of four pixels, its high bits, and the
/// output format.
const ZMOW: u8 = 0x5A;
const ZMOH: u8 = 0x5B;
const ZMHH: u8 = 0x5C;
const IMAGE_MODE: u8 = 0xDA;
const IMAGE_MODE_Y8: u8 = 0x40;
const IMAGE_MODE_JPEG: u8 = 0x10;
const IMAGE_MODE_LBYTE_FIRST: u8 = 0x01;

/// The frame rate: the clock and timing registers are not modelled.
const DEFAULT_FPS: f64 = 15.0;

/// The board's names for the sensor's pins: its 8-bit data bus (D0 is the
/// sensor's Y2), the pixel clock, VSYNC, HREF, the clock the chip gives it,
/// and SCCB's clock and data.
const DATA: [&str; 8] = ["D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"];

struct Ov2640 {
    pclk: Option<u32>,
    vsync: Option<u32>,
    href: Option<u32>,
    xclk: Option<u32>,
    data: [Option<u32>; 8],
    sioc: Option<u32>,
    siod: Option<u32>,
    addr: u32,
    /// The bank register 0xFF chose, and both banks' registers as written.
    bank: u8,
    dsp: [u8; 256],
    sensor: [u8; 256],
    /// The register an SCCB read starts at.
    pointer: u8,
    /// Its frames, rate and bars.
    camera: Sensor,
}

impl Default for Ov2640 {
    fn default() -> Self {
        Self {
            pclk: None,
            vsync: None,
            href: None,
            xclk: None,
            data: [None; 8],
            sioc: None,
            siod: None,
            addr: DEFAULT_ADDR,
            bank: BANK_DSP,
            dsp: [0; 256],
            sensor: [0; 256],
            pointer: 0,
            camera: Sensor::default(),
        }
    }
}

impl Ov2640 {
    /// The output the firmware programmed: `None` until it has set a size.
    fn mode(&self) -> Option<Mode> {
        let hh = self.dsp[ZMHH as usize];
        let width = ((u32::from(hh & 0x03) << 8) | u32::from(self.dsp[ZMOW as usize])) * 4;
        let height = ((u32::from(hh >> 2 & 0x01) << 8) | u32::from(self.dsp[ZMOH as usize])) * 4;
        let image = self.dsp[IMAGE_MODE as usize];
        if width == 0 || height == 0 {
            return None;
        }
        let name = if image & IMAGE_MODE_JPEG != 0 {
            b"JPEG"
        } else if image & IMAGE_MODE_Y8 != 0 {
            b"GREY"
        } else {
            let low_first = image & IMAGE_MODE_LBYTE_FIRST != 0;
            match (image >> 2 & 0x03, low_first) {
                (0, false) => b"YUYV",
                (0, true) => b"UYVY",
                (2, false) => b"RGBP",
                (2, true) => b"RGBR",
                _ => return None,
            }
        };
        Some(Mode { width, height, fourcc: fourcc(name) })
    }

    /// The registers of the bank chosen now.
    fn bank_mut(&mut self) -> &mut [u8; 256] {
        if self.bank == BANK_SENSOR {
            &mut self.sensor
        } else {
            &mut self.dsp
        }
    }

    fn read(&self, reg: u8) -> u8 {
        if reg == BANK_SEL {
            return self.bank;
        }
        if self.bank == BANK_SENSOR {
            SENSOR_ID.iter().find(|(r, _)| *r == reg).map_or(self.sensor[reg as usize], |(_, v)| *v)
        } else {
            self.dsp[reg as usize]
        }
    }

    fn write(&mut self, reg: u8, value: u8) {
        if reg == BANK_SEL {
            self.bank = value & 0x01;
            return;
        }
        if self.bank == BANK_SENSOR && reg == COM7 && value & COM7_SRST != 0 {
            // A soft reset: every register back to its power-on value.
            self.dsp = [0; 256];
            self.sensor = [0; 256];
            return;
        }
        self.bank_mut()[reg as usize] = value;
    }
}

impl Device for Ov2640 {
    const NAME: &'static str = "ov2640";
    const ON_BUS: bool = true;
    const ON_MESSAGE: bool = true;

    fn configure(&mut self, config: &PartConfig) {
        let pin = |name: &str| config.pins.get(name).copied();
        self.pclk = pin("PCLK");
        self.vsync = pin("VSYNC");
        self.href = pin("HREF");
        self.xclk = pin("XCLK");
        for (slot, name) in self.data.iter_mut().zip(DATA) {
            *slot = pin(name);
        }
        self.sioc = pin("SIOC");
        self.siod = pin("SIOD");
        self.addr = if config.addr == 0 { DEFAULT_ADDR } else { config.addr };
    }

    /// SCCB's address on the two pins wired to it, and the DVP bus once its
    /// clock, VSYNC and the eight data pins are wired. A board that wired
    /// neither gets a part that claims nothing.
    fn claims(&self) -> Vec<Claim> {
        let mut claims = Vec::new();
        if self.sioc.is_some() || self.siod.is_some() {
            let i2c = I2cClaim { addr: self.addr, scl_pin: self.sioc, sda_pin: self.siod };
            claims.push(Claim { what: Some(claim::What::I2c(i2c)) });
        }
        let data: Option<Vec<u32>> = self.data.iter().copied().collect();
        if let (Some(pclk), Some(vsync), Some(data)) = (self.pclk, self.vsync, data) {
            let data: [u32; 8] = data.try_into().expect("eight data pins");
            claims.push(dvp(pclk, vsync, data, self.href, self.xclk));
        }
        claims
    }

    /// An SCCB transaction: the first byte written is the register, the rest
    /// are stored from it on; a read answers from the register last named.
    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        let mut reply = BusReply { correlation_id: request.correlation_id, op: BusOp::I2c as i32, ..Default::default() };
        let Some(bus_request::Op::I2c(i2c)) = &request.op else { return Some(reply) };
        if i2c.addr != self.addr {
            return Some(reply);
        }
        if let Some((&reg, values)) = i2c.write.split_first() {
            self.pointer = reg;
            for &v in values {
                self.write(self.pointer, v);
                self.pointer = self.pointer.wrapping_add(1);
            }
        }
        reply.data = (0..i2c.read_len)
            .map(|_| {
                let v = self.read(self.pointer);
                self.pointer = self.pointer.wrapping_add(1);
                v
            })
            .collect();
        reply.ack = true;
        Some(reply)
    }

    fn on_dvp_format(&mut self, format: &DvpFormat, at_ns: u64) {
        self.camera.format(format.capturing, format.session, at_ns, DEFAULT_FPS);
    }

    /// The bars in the mode programmed now, a frame a period.
    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        let mode = self.mode();
        self.camera.poll(now_ns, DEFAULT_FPS, PART_BUFFER, mode)
    }

    /// What to show it: a picture of the mode programmed now, RGB888 or a
    /// JPEG, at its rate; nothing larger than its buffer takes.
    fn video_format(&mut self) -> Option<VideoFormat> {
        let mode = self.mode();
        self.camera.video_format(mode, DEFAULT_FPS, PART_BUFFER)
    }

    /// A picture for the mode programmed now, kept and sent as its frames.
    fn video_in(&mut self, picture: &[u8]) -> Result<(), String> {
        let mode = self.mode();
        self.camera.video_in(mode, picture)
    }

    /// `set:fps=<n>`: the frame rate, from 1 to 60 a second.
    fn on_input(&mut self, name: &str) {
        self.camera.on_input(name);
    }

    fn state(&self) -> Option<State> {
        Some(self.camera.state(self.mode(), DEFAULT_FPS))
    }
}

// A picture it is shown comes in whole: RGB888 up to VGA, or a JPEG.
esp_part::export_part!(Ov2640, buffer = 1048576, sees);

#[cfg(test)]
mod tests {
    use super::*;
    use esp_part::Payload;

    fn wired() -> Ov2640 {
        let mut sensor = Ov2640::default();
        let pins = [
            ("D0", 4), ("D1", 5), ("D2", 6), ("D3", 7), ("D4", 8), ("D5", 9), ("D6", 10), ("D7", 11),
            ("PCLK", 12), ("VSYNC", 13), ("HREF", 14), ("XCLK", 15), ("SIOC", 16), ("SIOD", 17),
        ];
        let config = PartConfig { pins: pins.iter().map(|(n, p)| (n.to_string(), *p)).collect(), ..Default::default() };
        sensor.configure(&config);
        sensor
    }

    fn sccb(sensor: &mut Ov2640, write: &[u8], read_len: u32) -> Vec<u8> {
        let i2c = I2cTransaction { bus: 0, addr: DEFAULT_ADDR, write: write.to_vec(), read_len, continues: false };
        let reply = sensor.on_bus(&BusRequest { correlation_id: 7, op: Some(bus_request::Op::I2c(i2c)) }).unwrap();
        assert!(reply.ack && reply.correlation_id == 7);
        reply.data
    }

    /// `esp_cam_sensor`'s 320 x 240 RGB565 setting, low byte first.
    fn program_qvga_rgb565_le(sensor: &mut Ov2640) {
        for w in [[0xFF, 0x00], [0xE0, 0x04], [0xDA, 0x09], [0x5A, 0x50], [0x5B, 0x3C], [0x5C, 0x00], [0xE0, 0x00]] {
            sccb(sensor, &w, 0);
        }
    }

    #[test]
    fn it_claims_sccb_and_the_dvp_bus_it_is_wired_to() {
        let claims = wired().claims();
        assert!(matches!(&claims[0].what, Some(claim::What::I2c(c)) if c.addr == 0x30 && c.scl_pin == Some(16)));
        assert!(matches!(&claims[1].what, Some(claim::What::Dvp(d))
            if d.pclk_pin == 12 && d.vsync_pin == 13 && d.de_pin == Some(14) && d.data_pins == [4, 5, 6, 7, 8, 9, 10, 11]));
        assert!(Ov2640::default().claims().is_empty(), "unwired, it claims nothing");
    }

    #[test]
    fn the_chip_id_is_in_the_sensor_bank() {
        let mut sensor = wired();
        sccb(&mut sensor, &[0xFF, 0x01], 0);
        assert_eq!(sccb(&mut sensor, &[0x0A], 2), [0x26, 0x42]);
        assert_eq!(sccb(&mut sensor, &[0x1C], 2), [0x7F, 0xA2]);
    }

    #[test]
    fn the_output_is_the_size_and_format_programmed() {
        let mut sensor = wired();
        assert_eq!(sensor.mode(), None, "nothing programmed");
        program_qvga_rgb565_le(&mut sensor);
        assert_eq!(sensor.mode(), Some(Mode { width: 320, height: 240, fourcc: fourcc(b"RGBR") }));
        // 1600 x 1200: the high bits in ZMHH.
        sccb(&mut sensor, &[0x5A, 0x90, 0x2C, 0x05], 0);
        assert_eq!(sensor.mode().map(|m| (m.width, m.height)), Some((1600, 1200)));
        sccb(&mut sensor, &[0xDA, 0x00], 0);
        assert_eq!(sensor.mode().map(|m| m.fourcc), Some(fourcc(b"YUYV")));
        sccb(&mut sensor, &[0xDA, 0x10], 0);
        assert_eq!(sensor.mode().map(|m| m.fourcc), Some(fourcc(b"JPEG")));
        // A soft reset forgets it all.
        sccb(&mut sensor, &[0xFF, 0x01], 0);
        sccb(&mut sensor, &[0x12, 0x80], 0);
        assert_eq!(sensor.mode(), None);
    }

    #[test]
    fn a_capturing_receiver_gets_the_bars_in_pieces_a_period_apart() {
        let mut sensor = wired();
        program_qvga_rgb565_le(&mut sensor);
        sensor.on_dvp_format(&DvpFormat { capturing: true, session: 1, ..Default::default() }, 1_000);
        let mut frame = Vec::<u8>::new();
        let start = loop {
            match sensor.poll(1_000) {
                Some(EmulatorMessage { sim_time_ns, payload: Some(Payload::CamFrame(f)) }) => {
                    frame.extend(&f.data);
                    if f.end_of_frame {
                        assert_eq!((f.width, f.height, f.fourcc, f.session), (320, 240, fourcc(b"RGBR"), 1));
                        break sim_time_ns;
                    }
                }
                other => panic!("{other:?}"),
            }
        };
        assert_eq!(start, 1_000 + 66_666_666, "a period after the format, at 15 fps");
        assert_eq!(frame.len(), 320 * 240 * 2);
        // The second bar, yellow, low byte first.
        assert_eq!(&frame[2 * 60..2 * 60 + 2], &[0xE0, 0xFF]);
        assert!(sensor.poll(1_000).is_none(), "the next frame is a period off");
        assert_eq!(sensor.state().unwrap().get("frames"), Some("1"));
    }

    #[test]
    fn in_jpeg_mode_it_asks_for_jpeg_and_sends_what_it_is_shown() {
        let mut sensor = wired();
        program_qvga_rgb565_le(&mut sensor);
        sccb(&mut sensor, &[0xDA, 0x10], 0);
        let f = sensor.video_format().unwrap();
        assert_eq!((f.width, f.height, f.fourcc, f.fps), (320, 240, fourcc(b"JPEG"), 15));
        sensor.on_dvp_format(&DvpFormat { capturing: true, session: 1, ..Default::default() }, 0);
        assert!(sensor.poll(0).is_none(), "no JPEG to send until it is shown one");
        let jpeg = [0xFF, 0xD8, 1, 2, 3, 0xFF, 0xD9];
        assert_eq!(sensor.video_in(&jpeg), Ok(()));
        match sensor.poll(66_666_666) {
            Some(EmulatorMessage { payload: Some(Payload::CamFrame(f)), .. }) => {
                assert_eq!((f.data.as_slice(), f.end_of_frame, f.fourcc), (&jpeg[..], true, fourcc(b"JPEG")));
            }
            other => panic!("{other:?}"),
        }
        // Back to RGB565: a picture of the wrong size is refused, and the
        // JPEG is still the last one shown.
        sccb(&mut sensor, &[0xDA, 0x09], 0);
        sensor.video_format();
        assert!(sensor.video_in(&[0; 10]).is_err());
        assert_eq!(sensor.state().unwrap().get("shown"), Some("7"));
    }

    #[test]
    fn a_picture_made_for_the_last_mode_is_not_taken_for_this_one() {
        let mut sensor = wired();
        program_qvga_rgb565_le(&mut sensor);
        sensor.video_format();
        // The firmware moves to JPEG before the RGB565 picture arrives.
        sccb(&mut sensor, &[0xDA, 0x10], 0);
        assert!(sensor.video_in(&vec![0x55; 320 * 240 * 3]).unwrap_err().contains("changed the mode"));
        assert_eq!(sensor.state().unwrap().get("shown"), Some("0"));
    }

    #[test]
    fn it_never_asks_for_a_picture_its_buffer_cannot_take() {
        let mut sensor = wired();
        program_qvga_rgb565_le(&mut sensor);
        assert!(sensor.video_format().is_some(), "320x240 RGB888 fits");
        sccb(&mut sensor, &[0x5A, 0x90, 0x2C, 0x05], 0); // 1600 x 1200
        assert!(sensor.video_format().is_none(), "1600x1200 RGB888 does not");
    }
}
