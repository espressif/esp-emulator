//! An Atmel AT93C46D three-wire serial EEPROM, in its 8-bit organisation,
//! written as a virtual part.
//!
//! A part is a WebAssembly module that models a component wired to the chip's
//! pins. This one is the EEPROM of IDF's `spi_master/hd_eeprom` example: 128
//! bytes behind a 10-bit instruction (a start bit, a 2-bit opcode and a 7-bit
//! address), selected by a chip select that is *high*, and busy for a few
//! milliseconds after a write, which it shows on its data output while the
//! chip select is raised again. It follows "AT93C46D Three-wire Serial EEPROM
//! 1-Kbit (128 x 8 or 64 x 16)" (Atmel 5140); README.md lists what it leaves
//! out.
//!
//! The example's driver works the chip select by hand (`spics_io_num = -1`)
//! because the busy signal is read with the chip select high and nothing
//! clocking, which an SPI transaction cannot do. So the part claims a chip
//! select that is a plain GPIO, says it selects when high, and names the bus
//! pads, which is how the emulator tells which bus the device is on.
//!
//! `Eeprom93c46` is the device and knows nothing about WebAssembly. The
//! `export_part!` line at the end makes it a part: the `esp-part` crate
//! exports every function of `proto/PART_ABI.md`, `part_on_bus` included, and
//! moves the bytes between the shared buffer and the device.

use esp_part::prelude::*;

/// The chip's own names for its pins (the datasheet's pin table). The board
/// says which chip pins they are wired to, as
/// `{ pins: { CS: 10, SK: 11, DI: 12, DO: 13 } }`.
const CS: &str = "CS";
const SK: &str = "SK";
const DI: &str = "DI";
const DO: &str = "DO";

/// 1 Kbit as 128 bytes: the 8-bit organisation (ORG low), which the example
/// uses.
const SIZE: usize = 128;

/// How long a write or erase cycle takes before the chip answers ready. The
/// datasheet's tWC is at most 5 ms; a typical part is faster.
const T_WC_NS: u64 = 2_000_000;

/// The instruction's opcodes, bits 9..7 of the 10-bit instruction: the start
/// bit, then the two opcode bits of the datasheet's instruction set table.
const OP_READ: u16 = 0b110;
const OP_WRITE: u16 = 0b101;
const OP_ERASE: u16 = 0b111;
/// EWEN, EWDS, ERAL and WRAL share this opcode and are told apart by the top
/// two address bits.
const OP_MISC: u16 = 0b100;

/// Where a write or erase cycle is. The driver drops the chip select after
/// the instruction and raises it again to read the busy signal: DO low from
/// the drop, high once the cycle is done, tWC after the rise.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Cycle {
    Idle,
    /// The instruction was taken; the chip select is still high for it.
    Written,
    /// The chip select dropped: busy, DO low until ready.
    Busy,
}

struct Eeprom93c46 {
    /// The chip pins the EEPROM's own pins are wired to. `None` until the
    /// board says, or when it left the pin unconnected.
    cs: Option<u32>,
    sk: Option<u32>,
    di: Option<u32>,
    do_: Option<u32>,
    /// The array. A fresh chip is erased: every byte 0xFF.
    mem: [u8; SIZE],
    /// EWEN was given and EWDS has not been since. A write or erase while
    /// disabled is ignored, as the chip ignores it.
    write_enabled: bool,
    cycle: Cycle,
    /// The chip select's level as last seen, to catch it moving.
    cs_high: bool,
    /// Drive DO low at the next poll: a cycle has started.
    send_low: bool,
    /// When the cycle ends and DO goes high, on the emulated clock; sent at
    /// the next poll, stamped with that time, so it lands then and not at
    /// the heartbeat after it.
    ready_at: Option<u64>,
}

impl Default for Eeprom93c46 {
    fn default() -> Self {
        Self {
            cs: None,
            sk: None,
            di: None,
            do_: None,
            mem: [0xFF; SIZE],
            write_enabled: false,
            cycle: Cycle::Idle,
            cs_high: false,
            send_low: false,
            ready_at: None,
        }
    }
}

impl Device for Eeprom93c46 {
    const NAME: &'static str = "eeprom93c46";
    const ON_BUS: bool = true;

    /// Where the board wired the part.
    fn configure(&mut self, config: &PartConfig) {
        self.cs = config.pins.get(CS).copied();
        self.sk = config.pins.get(SK).copied();
        self.di = config.pins.get(DI).copied();
        self.do_ = config.pins.get(DO).copied();
    }

    /// The chip select this EEPROM is selected by, high, with the bus pads
    /// beside it: the firmware drives the chip select as a plain GPIO, so
    /// the clock or a data pad is what tells the emulator which SPI bus the
    /// device is on. It drives DO, for the busy signal, and watches the chip
    /// select, so each move of it reaches the part at its time.
    fn claims(&self) -> Vec<Claim> {
        let spi = SpiClaim {
            cs_pin: self.cs,
            clk_pin: self.sk,
            mosi_pin: self.di,
            miso_pin: self.do_,
            cs_active_high: Some(true),
            ..Default::default()
        };
        let mut claims = vec![Claim { what: Some(claim::What::Spi(spi)) }];
        if let Some(do_) = self.do_ {
            claims.push(drives(1 << do_));
        }
        if let Some(cs) = self.cs {
            claims.push(watches(1 << cs));
        }
        claims
    }

    /// The chip select dropping after a write starts the cycle, with DO low
    /// for busy; rising again, it is read, and DO goes high tWC later.
    fn on_pins(&mut self, levels: u64, now_ns: u64) {
        let Some(cs) = self.cs else { return };
        let high = levels >> cs & 1 != 0;
        match self.cycle {
            Cycle::Written if !high => {
                self.cycle = Cycle::Busy;
                self.send_low = true;
            }
            Cycle::Busy if high && !self.cs_high => {
                self.ready_at = Some(now_ns + T_WC_NS);
            }
            _ => {}
        }
        self.cs_high = high;
    }

    /// The busy signal on DO: low as a cycle starts, now; high when it is
    /// done, stamped with that time for the emulator to land it then. A line
    /// nothing drives reads high, so high is also the rest.
    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        let do_ = self.do_?;
        if std::mem::take(&mut self.send_low) {
            return Some(drive(do_, false, now_ns));
        }
        if let Some(at) = self.ready_at.take() {
            self.cycle = Cycle::Idle;
            return Some(drive(do_, true, at.max(now_ns)));
        }
        None
    }

    /// The guest's SPI transfer to this chip: the instruction is the
    /// transfer's 10-bit command phase, the byte written is its data phase,
    /// and a READ gets bytes from the address on, as the chip clocks them
    /// out for as long as it is clocked. A transfer that reads nothing needs
    /// no answer; a read that is not a READ gets a line nobody drives.
    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        let Some(bus_request::Op::Spi(spi)) = &request.op else { return None };
        let answer = |ack: bool, data: Vec<u8>| {
            Some(BusReply { correlation_id: request.correlation_id, ack, data, op: BusOp::Spi as i32 })
        };
        let undriven = || (spi.rx_len > 0).then(|| answer(false, Vec::new())).flatten();
        let Some((op, addr)) = instruction(spi) else { return undriven() };
        match op {
            OP_READ => {
                let data = (0..spi.rx_len as usize).map(|i| self.mem[(addr + i) % SIZE]).collect();
                answer(true, data)
            }
            OP_WRITE => {
                if let (true, Some(&byte)) = (self.write_enabled, spi.tx.first()) {
                    self.mem[addr] = byte;
                    self.cycle = Cycle::Written;
                }
                undriven()
            }
            OP_ERASE => {
                if self.write_enabled {
                    self.mem[addr] = 0xFF;
                    self.cycle = Cycle::Written;
                }
                undriven()
            }
            OP_MISC => {
                // The datasheet's instruction set: the two address bits after
                // the opcode tell EWEN (11), EWDS (00), ERAL (10) and WRAL (01).
                match (addr >> 5, self.write_enabled) {
                    (0b11, _) => self.write_enabled = true,
                    (0b00, _) => self.write_enabled = false,
                    (0b10, true) => {
                        self.mem.fill(0xFF);
                        self.cycle = Cycle::Written;
                    }
                    (0b01, true) => {
                        if let Some(&byte) = spi.tx.first() {
                            self.mem.fill(byte);
                            self.cycle = Cycle::Written;
                        }
                    }
                    _ => {}
                }
                undriven()
            }
            // No start bit: not an instruction.
            _ => undriven(),
        }
    }

    /// `erase` returns the chip to its erased state, every byte 0xFF.
    fn on_input(&mut self, name: &str) {
        if name == "erase" {
            self.mem.fill(0xFF);
        }
    }

    fn state(&self) -> Option<State> {
        let busy = self.cycle != Cycle::Idle;
        Some(State::new().set("write-enabled", self.write_enabled).set("busy", busy))
    }
}

/// The opcode (bits 9..7) and address (bits 6..0) of the transfer's command
/// phase, which carries the 10-bit instruction left-aligned, first bit first.
/// `None` for a transfer without one, or with one of another length: not an
/// instruction this chip has.
fn instruction(spi: &SpiTransfer) -> Option<(u16, usize)> {
    let cmd = spi.cmd.as_ref()?;
    if cmd.bits != 10 || cmd.lines != 1 || cmd.data.len() < 2 {
        return None;
    }
    let word = u16::from_be_bytes([cmd.data[0], cmd.data[1]]) >> 6;
    Some((word >> 7, usize::from(word & 0x7F)))
}

// Big enough for the longest read the driver makes without DMA, 64 bytes, and
// the write a host hands over with it.
esp_part::export_part!(Eeprom93c46, buffer = 512);

// The device runs on the host too, so `cargo test` checks it without an
// emulator.
#[cfg(test)]
mod tests {
    use super::*;
    use esp_part::proto::SpiPhase;

    /// Wired as README.md's board wires it: CS on GPIO 10, SK on 11, DI on
    /// 12, DO on 13.
    fn chip() -> Eeprom93c46 {
        let mut chip = Eeprom93c46::default();
        let pins = [(CS.to_string(), 10), (SK.to_string(), 11), (DI.to_string(), 12), (DO.to_string(), 13)].into();
        chip.configure(&PartConfig { pins, ..Default::default() });
        chip
    }

    /// A transfer as IDF's driver clocks it for the example: the 10-bit
    /// instruction in the command phase, left-aligned in two bytes.
    fn transfer(instruction: u16, tx: &[u8], rx_len: u32) -> BusRequest {
        let cmd = SpiPhase { bits: 10, lines: 1, data: (instruction << 6).to_be_bytes().to_vec() };
        let spi = SpiTransfer { bus: 2, cmd: Some(cmd), tx: tx.to_vec(), rx_len, ..Default::default() };
        BusRequest { correlation_id: 7, op: Some(bus_request::Op::Spi(spi)) }
    }

    const READ: u16 = 0x300;
    const WRITE: u16 = 0x280;
    const ERASE: u16 = 0x380;
    const EWEN: u16 = 0x200 | 0x60;
    const EWDS: u16 = 0x200;
    const ERAL: u16 = 0x200 | 0x40;
    const WRAL: u16 = 0x200 | 0x20;

    fn read(chip: &mut Eeprom93c46, addr: u16, n: u32) -> Vec<u8> {
        chip.on_bus(&transfer(READ | addr, &[], n)).expect("a read is answered").data
    }

    #[test]
    fn a_fresh_chip_reads_erased() {
        assert_eq!(read(&mut chip(), 0, 3), [0xFF, 0xFF, 0xFF]);
    }

    #[test]
    fn a_write_takes_after_ewen_and_is_ignored_before() {
        let mut chip = chip();
        assert_eq!(chip.on_bus(&transfer(WRITE | 5, b"H", 0)), None, "a write needs no answer");
        assert_eq!(read(&mut chip, 5, 1), [0xFF], "ignored: writes are disabled");
        chip.on_bus(&transfer(EWEN, &[], 0));
        chip.on_bus(&transfer(WRITE | 5, b"H", 0));
        assert_eq!(read(&mut chip, 5, 1), [b'H']);
        chip.on_bus(&transfer(EWDS, &[], 0));
        chip.on_bus(&transfer(WRITE | 5, b"i", 0));
        assert_eq!(read(&mut chip, 5, 1), [b'H'], "disabled again");
    }

    #[test]
    fn a_read_continues_through_the_addresses_and_wraps() {
        let mut chip = chip();
        chip.on_bus(&transfer(EWEN, &[], 0));
        for (i, b) in b"Hello".iter().enumerate() {
            chip.on_bus(&transfer(WRITE | i as u16, &[*b], 0));
        }
        assert_eq!(read(&mut chip, 0, 6), b"Hello\xFF");
        assert_eq!(read(&mut chip, 127, 2), [0xFF, b'H'], "127 then 0");
    }

    #[test]
    fn erase_eral_and_wral_act_only_when_enabled() {
        let mut chip = chip();
        chip.on_bus(&transfer(EWEN, &[], 0));
        chip.on_bus(&transfer(WRAL, &[0x55], 0));
        assert_eq!(read(&mut chip, 100, 2), [0x55, 0x55]);
        chip.on_bus(&transfer(ERASE | 100, &[], 0));
        assert_eq!(read(&mut chip, 100, 2), [0xFF, 0x55]);
        chip.on_bus(&transfer(ERAL, &[], 0));
        assert_eq!(read(&mut chip, 101, 1), [0xFF]);
        chip.on_bus(&transfer(EWDS, &[], 0));
        chip.on_bus(&transfer(WRAL, &[0x55], 0));
        assert_eq!(read(&mut chip, 0, 1), [0xFF], "WRAL while disabled is ignored");
    }

    #[test]
    fn the_busy_signal_follows_the_chip_select_after_a_write() {
        let mut chip = chip();
        chip.on_bus(&transfer(EWEN, &[], 0));
        assert_eq!(chip.poll(0), None, "nothing to say before a write");
        chip.on_pins(1 << 10, 1_000); // the chip select high for the write
        chip.on_bus(&transfer(WRITE | 1, b"x", 0));
        assert_eq!(chip.poll(1_100), None, "nothing while the chip select is still high");
        chip.on_pins(0, 20_000); // the driver drops it after the transfer
        assert_eq!(chip.poll(20_000), Some(drive(13, false, 20_000)), "busy: DO low");
        assert_eq!(chip.state().unwrap().get("busy"), Some("true"));
        chip.on_pins(1 << 10, 21_000); // and raises it to read the busy signal
        assert_eq!(chip.poll(21_000), Some(drive(13, true, 2_021_000)), "ready tWC after the rise, stamped");
        assert_eq!(chip.poll(3_000_000), None);
        assert_eq!(chip.state().unwrap().get("busy"), Some("false"));
    }

    #[test]
    fn a_read_that_is_not_a_read_instruction_gets_an_undriven_line() {
        let mut chip = chip();
        let reply = chip.on_bus(&transfer(WRITE | 1, b"x", 1)).expect("a read is answered");
        assert!(!reply.ack);
        let no_command = SpiTransfer { bus: 2, rx_len: 1, ..Default::default() };
        let req = BusRequest { correlation_id: 7, op: Some(bus_request::Op::Spi(no_command)) };
        assert!(!chip.on_bus(&req).unwrap().ack, "no instruction at all");
    }

    #[test]
    fn it_claims_a_high_chip_select_with_the_bus_pads_and_drives_do() {
        let claims = chip().claims();
        let spi = SpiClaim {
            cs_pin: Some(10),
            clk_pin: Some(11),
            mosi_pin: Some(12),
            miso_pin: Some(13),
            cs_active_high: Some(true),
            ..Default::default()
        };
        assert_eq!(claims, [Claim { what: Some(claim::What::Spi(spi)) }, drives(1 << 13), watches(1 << 10)]);
        // Unwired: the chip select is left out and the handshake refuses it.
        let none = SpiClaim { cs_active_high: Some(true), ..Default::default() };
        assert_eq!(Eeprom93c46::default().claims(), [Claim { what: Some(claim::What::Spi(none)) }]);
    }

    #[test]
    fn erase_returns_it_to_erased() {
        let mut chip = chip();
        chip.on_bus(&transfer(EWEN, &[], 0));
        chip.on_bus(&transfer(WRITE | 3, b"x", 0));
        chip.on_input("erase");
        assert_eq!(read(&mut chip, 3, 1), [0xFF]);
    }
}
