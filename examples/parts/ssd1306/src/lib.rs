//! An SSD1306 128 x 64 monochrome OLED module on I2C, written as a virtual part
//! that draws.
//!
//! A part is a WebAssembly module that models a component wired to the chip's
//! pins. This one sits on the chip's I2C bus. Each transfer starts with a
//! control byte saying whether commands or display data follow; the data
//! lands in the controller's display RAM (GDDRAM), eight pages of 128 columns,
//! a byte eight pixels stacked down the page. It follows Solomon Systech's
//! "SSD1306" datasheet, revision 1.1. `part_render` hands the screen out a bit
//! a pixel; README.md gives the layout and lists what it leaves out.
//!
//! `Ssd1306` is the device and knows nothing about WebAssembly. The
//! `export_part!` line at the end makes it a part.

use esp_part::prelude::*;

/// The module's own names for its bus pins. The board says which chip pins
/// they are wired to, as `{ pins: { SCL: 6, SDA: 5 } }`.
const SCL: &str = "SCL";
const SDA: &str = "SDA";

/// The address with SA0 low. With it high the address is 0x3D, which the
/// board gives as `addr`.
const DEFAULT_ADDR: u32 = 0x3C;

const WIDTH: usize = 128;
const HEIGHT: usize = 64;
const PAGES: usize = HEIGHT / 8;
/// What `render` hands out: a bit a pixel, rows top to bottom, 16 bytes a
/// row, the most significant bit leftmost, 1 lit.
const PICTURE: usize = WIDTH * HEIGHT / 8;

/// The control byte (section 8.1.5.2). Co set: one byte follows, then
/// another control byte. D/C# set: the bytes are display data, not commands.
const CO: u8 = 0x80;
const DC: u8 = 0x40;

/// Where display data goes next (command 20h, section 10.1.3).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Addressing {
    Horizontal,
    Vertical,
    Page,
}

struct Ssd1306 {
    /// The chip pins `SCL` and `SDA` are wired to, `None` where the board
    /// left one unconnected.
    scl: Option<u32>,
    sda: Option<u32>,
    /// The 7-bit address it answers at.
    addr: u32,
    /// GDDRAM: eight pages of 128 columns, a byte eight rows, bit 0 on top.
    ram: [[u8; WIDTH]; PAGES],
    mode: Addressing,
    /// The column and page windows of horizontal and vertical addressing
    /// (21h, 22h), first and last, inclusive.
    cols: (usize, usize),
    pages: (usize, usize),
    /// The column start of page addressing (00h to 17h): where the column
    /// goes back to after column 127.
    page_col: usize,
    /// Where the next data byte goes.
    col: usize,
    page: usize,
    /// Display on (AFh), inverse (A7h), and every pixel lit (A5h).
    on: bool,
    inverse: bool,
    all_lit: bool,
    /// A command still owed parameter bytes, and how many.
    pending: Option<(u8, u8)>,
    /// The first two parameters of that command, all that any applied here
    /// takes.
    params: [u8; 2],
    got: usize,
    /// The screen changed since `render` last answered.
    changed: bool,
    picture: [u8; PICTURE],
}

impl Default for Ssd1306 {
    fn default() -> Self {
        Self {
            scl: None,
            sda: None,
            addr: DEFAULT_ADDR,
            ram: [[0; WIDTH]; PAGES],
            // Reset state (section 8.9): page addressing, full windows, the
            // display off.
            mode: Addressing::Page,
            cols: (0, WIDTH - 1),
            pages: (0, PAGES - 1),
            page_col: 0,
            col: 0,
            page: 0,
            on: false,
            inverse: false,
            all_lit: false,
            pending: None,
            params: [0; 2],
            got: 0,
            // The first `render` draws the dark panel.
            changed: true,
            picture: [0; PICTURE],
        }
    }
}

impl Device for Ssd1306 {
    const NAME: &'static str = "ssd1306";
    const ON_BUS: bool = true;

    /// Where the board wired the module, and the address it strapped it to.
    fn configure(&mut self, config: &PartConfig) {
        self.scl = config.pins.get(SCL).copied();
        self.sda = config.pins.get(SDA).copied();
        // Zero means the part's own default.
        self.addr = if config.addr == 0 { DEFAULT_ADDR } else { config.addr };
    }

    /// The address this module answers for, on the two pins it is wired to.
    /// A missing pin is left out, so the emulator refuses the claim and names
    /// what the board missed.
    fn claims(&self) -> Vec<Claim> {
        let i2c = I2cClaim { addr: self.addr, scl_pin: self.scl, sda_pin: self.sda };
        vec![Claim { what: Some(claim::What::I2c(i2c)) }]
    }

    /// A transfer to this module: what it writes is decoded. The module has
    /// nothing to read on I2C, so a read gets zeros, acknowledged.
    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        let (correlation_id, op) = (request.correlation_id, BusOp::I2c as i32);
        let mut reply = BusReply { correlation_id, op, ..Default::default() };
        // The emulator sends only transfers to this part's address; any other
        // is NACKed, as an empty address would be.
        let Some(bus_request::Op::I2c(i2c)) = &request.op else { return Some(reply) };
        if i2c.addr != self.addr {
            return Some(reply);
        }
        self.write(&i2c.write);
        reply.ack = true;
        reply.data = vec![0; i2c.read_len as usize];
        Some(reply)
    }

    /// Whether the display is on or inverted, and the layout `render` hands
    /// out.
    fn state(&self) -> Option<State> {
        Some(
            State::new()
                .set("on", self.on)
                .set("inverted", self.inverse)
                .set("width", WIDTH)
                .set("height", HEIGHT)
                .set("format", "mono"),
        )
    }

    /// The screen as the panel shows it, when it changed since the last call:
    /// dark while the display is off, every pixel lit after A5h, each pixel
    /// flipped while inverse.
    fn render(&mut self) -> Option<&[u8]> {
        if !std::mem::take(&mut self.changed) {
            return None;
        }
        self.picture = [0; PICTURE];
        if self.on {
            for y in 0..HEIGHT {
                for x in 0..WIDTH {
                    let set = self.ram[y / 8][x] >> (y % 8) & 1 == 1;
                    if self.all_lit || set != self.inverse {
                        self.picture[y * WIDTH / 8 + x / 8] |= 0x80 >> (x % 8);
                    }
                }
            }
        }
        Some(&self.picture[..])
    }
}

impl Ssd1306 {
    /// One transfer's bytes: control bytes, each followed by commands or
    /// display data, all the rest of the transfer when Co is clear and one
    /// byte when it is set.
    fn write(&mut self, bytes: &[u8]) {
        let mut rest = bytes;
        while let Some((&control, after)) = rest.split_first() {
            let take = if control & CO == 0 { after.len() } else { after.len().min(1) };
            for &b in &after[..take] {
                if control & DC == 0 {
                    self.command(b);
                } else {
                    self.data(b);
                }
            }
            rest = &after[take..];
        }
    }

    /// One command byte (Table 9-1). A parameter byte is taken by the command
    /// that is owed it, which may have come in an earlier transfer: a byte
    /// read as a command of its own instead (D3h 00h read as 00h, a lower
    /// column) moves the pointer and every write after it.
    fn command(&mut self, b: u8) {
        if let Some((cmd, owed)) = self.pending {
            if let Some(p) = self.params.get_mut(self.got) {
                *p = b;
            }
            self.got += 1;
            if owed > 1 {
                self.pending = Some((cmd, owed - 1));
            } else {
                self.pending = None;
                self.apply(cmd);
            }
            return;
        }
        match b {
            0xAE | 0xAF => {
                let on = b == 0xAF;
                self.changed |= self.on != on;
                self.on = on;
            }
            0xA6 | 0xA7 => {
                let inverse = b == 0xA7;
                self.changed |= self.inverse != inverse;
                self.inverse = inverse;
            }
            0xA4 | 0xA5 => {
                let all_lit = b == 0xA5;
                self.changed |= self.all_lit != all_lit;
                self.all_lit = all_lit;
            }
            // The column start of page addressing, a nibble each, which also
            // moves the pointer there.
            0x00..=0x0F => {
                self.page_col = (self.page_col & 0x70) | usize::from(b & 0x0F);
                self.col = self.page_col;
            }
            0x10..=0x17 => {
                self.page_col = (self.page_col & 0x0F) | (usize::from(b & 0x07) << 4);
                self.col = self.page_col;
            }
            // The page of page addressing; the mode is 20h's alone.
            0xB0..=0xB7 => self.page = usize::from(b & 0x07),
            // The display start line, the segment remap and the COM scan
            // direction: see README.md for why they are not applied.
            0x40..=0x7F | 0xA0 | 0xA1 | 0xC0 | 0xC8 => {}
            // Scrolling off and on, and the no-op.
            0x2E | 0x2F | 0xE3 => {}
            // Commands with parameters. Only 20h, 21h and 22h change where
            // data goes; the others (contrast, multiplex, offset, clock,
            // pre-charge, COM pins, VCOMH, charge pump, the scroll setups)
            // are consumed.
            0x20 | 0x81 | 0x8D | 0xA8 | 0xD3 | 0xD5 | 0xD9 | 0xDA | 0xDB => self.owe(b, 1),
            0x21 | 0x22 | 0xA3 => self.owe(b, 2),
            0x29 | 0x2A => self.owe(b, 5),
            0x26 | 0x27 => self.owe(b, 6),
            _ => {}
        }
    }

    fn owe(&mut self, cmd: u8, n: u8) {
        self.pending = Some((cmd, n));
        self.got = 0;
    }

    /// A command whose parameters are all in.
    fn apply(&mut self, cmd: u8) {
        let [p0, p1] = self.params;
        match cmd {
            // 11b is invalid; the controller keeps page addressing then.
            0x20 => {
                self.mode = match p0 & 0x03 {
                    0 => Addressing::Horizontal,
                    1 => Addressing::Vertical,
                    _ => Addressing::Page,
                }
            }
            // A window also puts the pointer at its start, so a driver sets it
            // once and sends a whole frame.
            0x21 => {
                self.cols = (usize::from(p0 & 0x7F), usize::from(p1 & 0x7F));
                self.col = self.cols.0;
            }
            0x22 => {
                self.pages = (usize::from(p0 & 0x07), usize::from(p1 & 0x07));
                self.page = self.pages.0;
            }
            _ => {}
        }
    }

    /// One byte of display data, at the pointer, which then moves on.
    fn data(&mut self, b: u8) {
        if let Some(cell) = self.ram.get_mut(self.page).and_then(|p| p.get_mut(self.col)) {
            self.changed |= *cell != b;
            *cell = b;
        }
        match self.mode {
            // Along the column window, then down the page window, wrapping.
            Addressing::Horizontal => {
                if self.col >= self.cols.1 {
                    self.col = self.cols.0;
                    self.page = if self.page >= self.pages.1 { self.pages.0 } else { self.page + 1 };
                } else {
                    self.col += 1;
                }
            }
            Addressing::Vertical => {
                if self.page >= self.pages.1 {
                    self.page = self.pages.0;
                    self.col = if self.col >= self.cols.1 { self.cols.0 } else { self.col + 1 };
                } else {
                    self.page += 1;
                }
            }
            // Along the page; after column 127 back to the column start, on
            // the same page (section 10.1.3).
            Addressing::Page => {
                self.col = if self.col >= WIDTH - 1 { self.page_col } else { self.col + 1 };
            }
        }
    }
}

// A frame is one write of 1025 bytes, a request of about 1040, and the
// picture 1024 bytes: both fit. A host refuses a request larger than the
// buffer, and the part fails.
esp_part::export_part!(Ssd1306, buffer = 2048);

// The device runs on the host too, so `cargo test` checks it without an
// emulator.
#[cfg(test)]
mod tests {
    use super::*;

    /// test_apps/oled_button's bring-up: every command with a parameter sends
    /// it in the same transfer.
    const INIT: &[u8] = &[
        0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
    ];

    /// "H" as that firmware draws it, a column a byte, bit 0 on top.
    const H: [u8; 5] = [0x7F, 0x08, 0x08, 0x08, 0x7F];

    fn wired(addr: u32) -> Ssd1306 {
        let mut oled = Ssd1306::default();
        oled.configure(&PartConfig { pins: [(SCL.to_string(), 6), (SDA.to_string(), 5)].into(), addr, ..Default::default() });
        oled
    }

    fn request(addr: u32, write: &[u8], read_len: u32) -> BusRequest {
        let i2c = I2cTransaction { bus: 0, addr, write: write.to_vec(), read_len, continues: false };
        BusRequest { correlation_id: 7, op: Some(bus_request::Op::I2c(i2c)) }
    }

    /// One transfer to the module's address: the bytes read back, or `None`
    /// for a NACK.
    fn send(oled: &mut Ssd1306, write: &[u8]) -> Option<Vec<u8>> {
        let reply = oled.on_bus(&request(oled.addr, write, 0)).unwrap();
        assert_eq!(reply.correlation_id, 7);
        reply.ack.then_some(reply.data)
    }

    /// A frame as the firmware sends one: the window, then 1024 bytes of
    /// data, `fb` page by page.
    fn frame(oled: &mut Ssd1306, fb: &[[u8; WIDTH]; PAGES]) {
        send(oled, &[0x00, 0x21, 0, 127, 0x22, 0, 7]);
        let mut data = vec![0x40];
        fb.iter().for_each(|page| data.extend_from_slice(page));
        send(oled, &data);
    }

    fn pixel(picture: &[u8], x: usize, y: usize) -> bool {
        picture[y * WIDTH / 8 + x / 8] & 0x80 >> (x % 8) != 0
    }

    fn lit(picture: &[u8]) -> u32 {
        picture.iter().map(|b| b.count_ones()).sum()
    }

    /// An "H" at column 4 of page 2, as the firmware's "HELLO" starts.
    fn hello() -> [[u8; WIDTH]; PAGES] {
        let mut fb = [[0; WIDTH]; PAGES];
        fb[2][4..9].copy_from_slice(&H);
        fb
    }

    #[test]
    fn a_frame_lands_where_the_firmware_drew_it() {
        let mut oled = wired(0);
        send(&mut oled, INIT);
        frame(&mut oled, &hello());
        let picture = oled.render().unwrap().to_vec();
        assert_eq!(picture.len(), 1024);
        // The H's two stems are rows 16 to 22 of columns 4 and 8; its bar is
        // row 19.
        for y in 16..23 {
            assert!(pixel(&picture, 4, y) && pixel(&picture, 8, y), "row {y}");
        }
        assert!(!pixel(&picture, 4, 23) && !pixel(&picture, 4, 15));
        assert!(pixel(&picture, 6, 19) && !pixel(&picture, 6, 18));
        assert_eq!(lit(&picture), 7 + 7 + 3);
    }

    #[test]
    fn a_still_screen_is_drawn_once() {
        let mut oled = wired(0);
        assert_eq!(oled.render().map(lit), Some(0), "the dark panel, at the first call");
        assert_eq!(oled.render(), None);
        send(&mut oled, INIT);
        assert!(oled.render().is_some(), "switched on");
        frame(&mut oled, &hello());
        assert!(oled.render().is_some());
        // The same frame again changes nothing on the panel.
        frame(&mut oled, &hello());
        assert_eq!(oled.render(), None);
    }

    #[test]
    fn off_inverse_and_every_pixel_lit_are_what_the_panel_shows() {
        let mut oled = wired(0);
        send(&mut oled, &[0x00, 0x20, 0x00]);
        frame(&mut oled, &hello());
        assert_eq!(oled.render().map(lit), Some(0), "off: dark, whatever GDDRAM holds");
        send(&mut oled, &[0x80, 0xAF]);
        assert_eq!(oled.render().map(lit), Some(17));
        send(&mut oled, &[0x80, 0xA7]);
        assert_eq!(oled.render().map(lit), Some(128 * 64 - 17), "inverse");
        send(&mut oled, &[0x80, 0xA6, 0x80, 0xA5]);
        assert_eq!(oled.render().map(lit), Some(128 * 64), "every pixel lit");
        send(&mut oled, &[0x00, 0xA4]);
        assert_eq!(oled.render().map(lit), Some(17));
    }

    #[test]
    fn page_addressing_stays_on_its_page() {
        let mut oled = wired(0);
        send(&mut oled, &[0x00, 0xAF, 0xB3, 0x0E, 0x17]);
        // Column 126: two bytes reach the end of page 3, the third goes back
        // to the column start, 126, on the same page.
        send(&mut oled, &[0x40, 0x01, 0x02, 0x04]);
        let picture = oled.render().unwrap();
        assert!(pixel(picture, 126, 26), "0x04 over 0x01: row 2 of page 3");
        assert!(!pixel(picture, 126, 24));
        assert!(pixel(picture, 127, 25));
        assert_eq!(lit(picture), 2);
    }

    #[test]
    fn vertical_addressing_goes_down_the_page_window() {
        let mut oled = wired(0);
        send(&mut oled, &[0x00, 0xAF, 0x20, 0x01, 0x21, 10, 11, 0x22, 6, 7]);
        send(&mut oled, &[0x40, 0x01, 0x01, 0x01]);
        let picture = oled.render().unwrap();
        assert!(pixel(picture, 10, 48) && pixel(picture, 10, 56) && pixel(picture, 11, 48));
        assert_eq!(lit(picture), 3);
    }

    #[test]
    fn a_parameter_is_never_read_as_a_command() {
        let mut oled = wired(0);
        // D3h's 00h, sent in a transfer of its own, is the display offset,
        // not "lower column 0"; nor are the scroll setup's six bytes.
        send(&mut oled, &[0x00, 0xAF, 0xB0, 0x05, 0xD3]);
        send(&mut oled, &[0x00, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF]);
        send(&mut oled, &[0x40, 0x80]);
        let picture = oled.render().unwrap();
        assert!(pixel(picture, 5, 7), "data went to column 5");
        assert_eq!(lit(picture), 1);
    }

    #[test]
    fn it_answers_at_its_address_and_a_read_gets_zeros() {
        let mut oled = wired(0x3D);
        assert_eq!(oled.claims(), vec![Claim {
            what: Some(claim::What::I2c(I2cClaim { addr: 0x3D, scl_pin: Some(6), sda_pin: Some(5) }))
        }]);
        assert_eq!(send(&mut oled, &[]), Some(vec![]), "a probe is acknowledged");
        let reply = oled.on_bus(&request(0x3D, &[], 2)).unwrap();
        assert!(reply.ack);
        assert_eq!(reply.data, [0, 0]);
        assert!(!oled.on_bus(&request(0x3C, &[0x80, 0xAF], 0)).unwrap().ack);
        assert!(!oled.on, "a transfer to another address changes nothing");
    }

    #[test]
    fn it_shows_its_layout() {
        let state = wired(0).state().unwrap();
        assert_eq!(state.get("width"), Some("128"));
        assert_eq!(state.get("height"), Some("64"));
        assert_eq!(state.get("format"), Some("mono"));
        assert_eq!(state.get("on"), Some("false"));
    }
}
