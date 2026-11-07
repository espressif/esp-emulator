//! A Sensirion SHT31-D temperature and humidity sensor, written as a virtual
//! part.
//!
//! A part is a WebAssembly module that models a component wired to the chip's
//! pins. This one sits on the chip's I2C bus. The firmware writes a 16-bit
//! command, then reads the answer: 16-bit words, each followed by its CRC. It
//! follows Sensirion's "Datasheet SHT3x-DIS", version 7, but models only the
//! single-shot measurement, the status register and the soft reset; README.md
//! lists what it leaves out.
//!
//! `Sht31` is the device and knows nothing about WebAssembly. The
//! `export_part!` line at the end makes it a part: the `esp-part` crate
//! exports every function of `proto/PART_ABI.md`, `part_on_bus` included, and
//! moves the bytes between the shared buffer and the device.

use esp_part::prelude::*;

/// The sensor's own names for its bus pins. The board says which chip pins
/// they are wired to, as `{ pins: { SCL: 6, SDA: 5 } }`.
const SCL: &str = "SCL";
const SDA: &str = "SDA";

/// The address with the ADDR pin low. With it high the address is 0x45, which
/// the board gives as `addr`.
const DEFAULT_ADDR: u32 = 0x44;

/// The status register after power-on and after a soft reset: bit 15 (alert
/// pending) and bit 4 (reset detected) set (Table 18).
const STATUS_AT_RESET: u16 = 0x8010;
/// The flags Clear Status clears: bits 15, 11, 10 and 4 (Table 19).
const STATUS_CLEARABLE: u16 = 0x8C10;
/// Bit 1: the last command was not processed (Table 18).
const STATUS_COMMAND_FAILED: u16 = 1 << 1;

struct Sht31 {
    /// The chip pins `SCL` and `SDA` are wired to. `None` until the board
    /// says, or when it left the pin unconnected: a sensor wired to neither
    /// answers for nothing.
    scl: Option<u32>,
    sda: Option<u32>,
    /// The 7-bit address it answers at.
    addr: u32,
    /// The air around the sensor, in degrees Celsius and percent relative
    /// humidity.
    temp_c: f32,
    humidity: f32,
    /// The status register (Table 18).
    status: u16,
    /// What the next read gets, CRCs included: a measurement's six bytes or the
    /// status word's three, taken when the command came. `None` when nothing
    /// was asked for or the answer was already read.
    pending: Option<Vec<u8>>,
    /// The rest of the answer a read has started, which a request that
    /// `continues` that read clocks out next.
    reading: Vec<u8>,
}

impl Default for Sht31 {
    fn default() -> Self {
        Self {
            scl: None,
            sda: None,
            addr: DEFAULT_ADDR,
            // Room conditions, so a fresh sensor reads plausibly.
            temp_c: 22.5,
            humidity: 45.0,
            status: STATUS_AT_RESET,
            pending: None,
            reading: Vec::new(),
        }
    }
}

impl Device for Sht31 {
    const NAME: &'static str = "sht31";
    const ON_BUS: bool = true;

    /// Where the board wired the part, and the address it strapped it to.
    fn configure(&mut self, config: &PartConfig) {
        self.scl = config.pins.get(SCL).copied();
        self.sda = config.pins.get(SDA).copied();
        // Zero means the part's own default.
        self.addr = if config.addr == 0 { DEFAULT_ADDR } else { config.addr };
    }

    /// The address this sensor answers for, on the two pins it is wired to.
    /// The emulator works out the I2C port from those pins. A sensor the
    /// board wired to one pin, or to none (a pin named `scl` rather than
    /// `SCL` is not one of its pins), still claims, with the missing pins
    /// left out, so the emulator refuses the claim and names what the board
    /// missed.
    fn claims(&self) -> Vec<Claim> {
        let i2c = I2cClaim { addr: self.addr, scl_pin: self.scl, sda_pin: self.sda };
        vec![Claim { what: Some(claim::What::I2c(i2c)) }]
    }

    /// Somebody changed the air around the sensor. The names are the part's
    /// own, and `set:temperature=<°C>` or `set:humidity=<%RH>` puts a value
    /// in one step.
    fn on_input(&mut self, name: &str) {
        if let Some((what, value)) = set_value(name) {
            match what {
                "temperature" => self.temp_c = value as f32,
                "humidity" => self.humidity = value as f32,
                _ => {} // Not a value this part has.
            }
        } else {
            match name {
                "warmer" => self.temp_c += 1.0,
                "cooler" => self.temp_c -= 1.0,
                "wetter" => self.humidity += 5.0,
                "drier" => self.humidity -= 5.0,
                _ => {} // Not an interaction this part has.
            }
        }
        // Kept to what a reading can carry: raw 0 to 65535 is -45 to 130 °C,
        // and 0 to 100 %RH.
        self.temp_c = self.temp_c.clamp(-45.0, 130.0);
        self.humidity = self.humidity.clamp(0.0, 100.0);
    }

    /// The guest's I2C transaction to this part: the command in `write`, if
    /// there is one, is acted on first, then the read, if any, is answered.
    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        let (correlation_id, op) = (request.correlation_id, BusOp::I2c as i32);
        let mut reply = BusReply { correlation_id, op, ..Default::default() };
        // The emulator sends only transactions to this part's address. Any
        // other is answered as an empty address would be, with a NACK.
        let Some(bus_request::Op::I2c(i2c)) = &request.op else { return Some(reply) };
        if i2c.addr != self.addr {
            return Some(reply);
        }
        // A command is two bytes, most significant first.
        if let &[hi, lo, ..] = i2c.write.as_slice() {
            self.command(u16::from_be_bytes([hi, lo]));
        }
        if i2c.read_len > 0 {
            if !i2c.continues {
                // A read after a START takes the answer. The sensor NACKs one
                // when it has no data (section 4.4).
                let Some(data) = self.pending.take() else {
                    self.reading.clear();
                    return Some(reply);
                };
                self.reading = data;
            }
            // A read that continues another, split by the master's driver,
            // carries on where that one stopped. The master decides how many
            // bytes it clocks: past the answer nothing drives the line, and it
            // reads 0xFF.
            let n = (i2c.read_len as usize).min(self.reading.len());
            let mut data: Vec<u8> = self.reading.drain(..n).collect();
            data.resize(i2c.read_len as usize, 0xFF);
            reply.data = data;
        }
        // A probe, a command on its own, or a read with an answer.
        reply.ack = true;
        Some(reply)
    }

    fn state(&self) -> Option<State> {
        Some(State::new().set("temperature", self.temp_c).set("humidity", self.humidity))
    }
}

impl Sht31 {
    /// Act on a 16-bit command. An unknown one is acknowledged, changes
    /// nothing, and sets the status register's command-failed bit, which the
    /// next known command clears.
    fn command(&mut self, code: u16) {
        let known = match code {
            // Single shot (Table 9), at high, medium and low repeatability,
            // with clock stretching and then without. The result is ready at
            // once: timing is not modelled. The real sensor takes up to 15 ms,
            // and without clock stretching it NACKs a read until then.
            0x2C06 | 0x2C0D | 0x2C10 | 0x2400 | 0x240B | 0x2416 => {
                self.pending = Some(self.measurement());
                true
            }
            // Read the status register.
            0xF32D => {
                self.pending = Some(word(self.status).to_vec());
                true
            }
            // Clear the status register's flags.
            0x3041 => {
                self.status &= !STATUS_CLEARABLE;
                true
            }
            // Soft reset: back to the power-on state. A pending answer is lost.
            0x30A2 => {
                self.status = STATUS_AT_RESET;
                self.pending = None;
                self.reading.clear();
                true
            }
            _ => false,
        };
        if known {
            self.status &= !STATUS_COMMAND_FAILED;
        } else {
            self.status |= STATUS_COMMAND_FAILED;
        }
    }

    /// A measurement as the sensor sends it: temperature, then humidity. The
    /// datasheet's conversions, T = -45 + 175 * raw / 65535 °C and
    /// RH = 100 * raw / 65535 %, inverted.
    fn measurement(&self) -> Vec<u8> {
        let t = (self.temp_c + 45.0) / 175.0 * 65535.0;
        let rh = self.humidity / 100.0 * 65535.0;
        [t, rh].into_iter().flat_map(|raw| word(raw.round().clamp(0.0, 65535.0) as u16)).collect()
    }
}

/// A 16-bit word as the sensor sends it: most significant byte first, then the
/// CRC of the two.
fn word(value: u16) -> [u8; 3] {
    let [hi, lo] = value.to_be_bytes();
    [hi, lo, crc8(&[hi, lo])]
}

/// The sensor's checksum: CRC-8, polynomial 0x31 (x^8 + x^5 + x^4 + 1),
/// initial value 0xFF, no final XOR (section 4.12).
fn crc8(bytes: &[u8]) -> u8 {
    let mut crc = 0xFF;
    for &byte in bytes {
        crc ^= byte;
        for _ in 0..8 {
            crc = if crc & 0x80 != 0 { (crc << 1) ^ 0x31 } else { crc << 1 };
        }
    }
    crc
}

// Big enough for an SHT31 driver's transactions, and for the reply to any
// read: the emulator asks for at most 255 bytes at a time. A host refuses a
// request larger than the buffer, a long write for instance, and the part
// fails.
esp_part::export_part!(Sht31, buffer = 512);

// The device runs on the host too, so `cargo test` checks it without an
// emulator.
#[cfg(test)]
mod tests {
    use super::*;

    /// What a fresh sensor measures, 22.5 °C and 45 %RH: raw 0x62BE and
    /// 0x7333, each followed by its CRC.
    const ROOM: [u8; 6] = [0x62, 0xBE, 0xAD, 0x73, 0x33, 0x01];

    /// The status word at power-on, 0x8010, and its CRC.
    const POWER_ON_STATUS: [u8; 3] = [0x80, 0x10, 0xE1];

    /// Wired as README.md's board wires it, SCL on GPIO 6 and SDA on GPIO 5,
    /// and strapped to `addr`.
    fn wired(addr: u32) -> Sht31 {
        let mut sensor = Sht31::default();
        sensor.configure(&PartConfig { pins: [(SCL.to_string(), 6), (SDA.to_string(), 5)].into(), addr });
        sensor
    }

    /// A sensor wired at its default address.
    fn sensor() -> Sht31 {
        wired(0)
    }

    fn request(addr: u32, write: &[u8], read_len: u32) -> BusRequest {
        let i2c = I2cTransaction { bus: 0, addr, write: write.to_vec(), read_len, continues: false };
        BusRequest { correlation_id: 0, op: Some(bus_request::Op::I2c(i2c)) }
    }

    /// One transaction to the sensor's address: the bytes read back, or `None`
    /// for a NACK.
    fn transfer(sensor: &mut Sht31, write: &[u8], read_len: u32) -> Option<Vec<u8>> {
        let reply = sensor.on_bus(&request(sensor.addr, write, read_len)).unwrap();
        reply.ack.then_some(reply.data)
    }

    #[test]
    fn the_crc_matches_the_datasheet_example() {
        // Section 4.12: the CRC of 0xBEEF is 0x92.
        assert_eq!(crc8(&[0xBE, 0xEF]), 0x92);
    }

    #[test]
    fn every_single_shot_command_measures_six_bytes() {
        for command in [[0x24, 0x00], [0x24, 0x0B], [0x24, 0x16], [0x2C, 0x06], [0x2C, 0x0D], [0x2C, 0x10]] {
            let mut sensor = sensor();
            assert_eq!(transfer(&mut sensor, &command, 0), Some(vec![]), "{command:02X?} not acknowledged");
            assert_eq!(transfer(&mut sensor, &[], 6), Some(ROOM.to_vec()), "{command:02X?}");
        }
    }

    #[test]
    fn a_read_takes_the_answer_and_a_read_with_none_is_nacked() {
        let mut sensor = sensor();
        assert_eq!(transfer(&mut sensor, &[], 6), None, "nothing was asked for");
        transfer(&mut sensor, &[0x24, 0x00], 0);
        assert_eq!(transfer(&mut sensor, &[], 6), Some(ROOM.to_vec()));
        assert_eq!(transfer(&mut sensor, &[], 6), None, "the answer was read once");
    }

    #[test]
    fn a_command_and_its_read_can_share_a_transaction() {
        let mut sensor = sensor();
        assert_eq!(transfer(&mut sensor, &[0x2C, 0x06], 6), Some(ROOM.to_vec()));
        assert_eq!(transfer(&mut sensor, &[0xF3, 0x2D], 3), Some(POWER_ON_STATUS.to_vec()));
    }

    #[test]
    fn the_status_reads_0x8010_until_cleared_and_again_after_a_soft_reset() {
        let mut sensor = sensor();
        assert_eq!(transfer(&mut sensor, &[0xF3, 0x2D], 3), Some(POWER_ON_STATUS.to_vec()));
        assert_eq!(transfer(&mut sensor, &[0x30, 0x41], 0), Some(vec![]));
        assert_eq!(transfer(&mut sensor, &[0xF3, 0x2D], 3), Some(vec![0x00, 0x00, 0x81]), "cleared");
        transfer(&mut sensor, &[0x24, 0x00], 0);
        assert_eq!(transfer(&mut sensor, &[0x30, 0xA2], 0), Some(vec![]));
        assert_eq!(transfer(&mut sensor, &[], 6), None, "the reset dropped the measurement");
        assert_eq!(transfer(&mut sensor, &[0xF3, 0x2D], 3), Some(POWER_ON_STATUS.to_vec()), "reset");
    }

    #[test]
    fn a_probe_and_an_unknown_command_are_acknowledged() {
        let mut sensor = sensor();
        assert_eq!(transfer(&mut sensor, &[], 0), Some(vec![]), "a probe");
        transfer(&mut sensor, &[0x24, 0x00], 0);
        assert_eq!(transfer(&mut sensor, &[0x12, 0x34], 0), Some(vec![]), "an unknown command");
        assert_eq!(transfer(&mut sensor, &[], 6), Some(ROOM.to_vec()), "which changed nothing");
        // But the status says it was not processed, until a known command.
        assert_eq!(transfer(&mut sensor, &[0xF3, 0x2D], 3), Some(word(0x8012).to_vec()));
        assert_eq!(transfer(&mut sensor, &[0xF3, 0x2D], 3), Some(POWER_ON_STATUS.to_vec()));
    }

    #[test]
    fn a_read_split_by_the_driver_continues_where_it_stopped() {
        // The legacy driver ends every READ with END, so a measurement read
        // comes as five bytes and then the last one, which `continues`.
        let mut sensor = sensor();
        assert_eq!(transfer(&mut sensor, &[0x24, 0x00], 5), Some(ROOM[..5].to_vec()));
        let mut rest = request(0x44, &[], 1);
        if let Some(bus_request::Op::I2c(i2c)) = &mut rest.op {
            i2c.continues = true;
        }
        let reply = sensor.on_bus(&rest).unwrap();
        assert_eq!((reply.ack, reply.data), (true, ROOM[5..].to_vec()));
        assert_eq!(transfer(&mut sensor, &[], 6), None, "a new read after it finds nothing");
    }

    #[test]
    fn a_read_of_another_length_is_cut_or_padded() {
        let mut sensor = sensor();
        assert_eq!(transfer(&mut sensor, &[0x24, 0x00], 2), Some(ROOM[..2].to_vec()));
        let padded = [&ROOM[..], &[0xFF, 0xFF]].concat();
        assert_eq!(transfer(&mut sensor, &[0x24, 0x00], 8), Some(padded));
    }

    #[test]
    fn it_claims_its_address_on_the_pins_it_is_wired_to_and_answers_there() {
        let i2c = |addr| Claim {
            what: Some(claim::What::I2c(I2cClaim { addr, scl_pin: Some(6), sda_pin: Some(5) })),
        };
        assert_eq!(sensor().claims(), [i2c(0x44)]);

        // The ADDR pin high: the board says 0x45.
        let mut sensor = wired(0x45);
        assert_eq!(sensor.claims(), [i2c(0x45)]);
        assert!(sensor.on_bus(&request(0x45, &[], 0)).unwrap().ack);
        assert!(!sensor.on_bus(&request(0x44, &[], 0)).unwrap().ack, "0x44 is not its address");

        // Unwired, or one pin wired: claimed with what is missing left out,
        // so the handshake refuses it and names the missing pins.
        let none = I2cClaim { addr: 0x44, scl_pin: None, sda_pin: None };
        assert_eq!(Sht31::default().claims(), [Claim { what: Some(claim::What::I2c(none)) }]);
        let mut one_pin = Sht31::default();
        one_pin.configure(&PartConfig { pins: [(SCL.to_string(), 6)].into(), ..Default::default() });
        let half = I2cClaim { addr: 0x44, scl_pin: Some(6), sda_pin: None };
        assert_eq!(one_pin.claims(), [Claim { what: Some(claim::What::I2c(half)) }]);
    }

    #[test]
    fn the_inputs_move_the_reading_and_stop_at_its_range() {
        let mut sensor = sensor();
        sensor.on_input("warmer");
        sensor.on_input("drier");
        sensor.on_input("wiggle");
        // 23.5 °C is raw 0x6434, 40 %RH raw 0x6666.
        assert_eq!(transfer(&mut sensor, &[0x24, 0x00], 6), Some(vec![0x64, 0x34, 0x56, 0x66, 0x66, 0x93]));
        for _ in 0..200 {
            sensor.on_input("warmer");
            sensor.on_input("wetter");
        }
        let top = vec![0xFF, 0xFF, 0xAC, 0xFF, 0xFF, 0xAC];
        assert_eq!(transfer(&mut sensor, &[0x24, 0x00], 6), Some(top), "130 °C and 100 %RH");
        for _ in 0..200 {
            sensor.on_input("cooler");
            sensor.on_input("drier");
        }
        let bottom = vec![0x00, 0x00, 0x81, 0x00, 0x00, 0x81];
        assert_eq!(transfer(&mut sensor, &[0x24, 0x00], 6), Some(bottom), "-45 °C and 0 %RH");
    }

    #[test]
    fn a_set_puts_an_exact_value_and_stays_in_range() {
        let mut sensor = sensor();
        sensor.on_input("set:temperature=31.5");
        sensor.on_input("set:humidity=62");
        // 31.5 °C is raw 0x6FE8, 62 %RH raw 0x9EB8.
        assert_eq!(transfer(&mut sensor, &[0x24, 0x00], 6).map(|b| [b[0], b[1], b[3], b[4]]),
            Some([0x6F, 0xE8, 0x9E, 0xB8]));
        sensor.on_input("set:temperature=-60");
        sensor.on_input("set:humidity=101.5");
        assert_eq!((sensor.temp_c, sensor.humidity), (-45.0, 100.0), "clamped to the range");
        for odd in ["set:temperature=1e2", "set:temperature=", "set:temperature=.5", "set:temperature=5.",
            "set:temperature=abc", "set:Temperature=5", "set:pressure=5", "set:temperature"] {
            sensor.on_input(odd);
        }
        assert_eq!((sensor.temp_c, sensor.humidity), (-45.0, 100.0), "anything else is ignored");
    }

    #[test]
    fn it_shows_what_it_was_set_to() {
        let mut sensor = sensor();
        sensor.on_input("set:temperature=31.5");
        let shown = sensor.state().unwrap();
        assert_eq!((shown.get("temperature"), shown.get("humidity")), (Some("31.5"), Some("45")));
    }

    #[test]
    fn the_reply_echoes_the_correlation_id_and_names_i2c() {
        let mut sensor = sensor();
        for (write, ack) in [(&[0x24, 0x00][..], true), (&[][..], false)] {
            let mut req = request(0x44, write, 6);
            req.correlation_id = 1;
            let reply = sensor.on_bus(&req).unwrap();
            assert_eq!((reply.correlation_id, reply.op, reply.ack), (1, BusOp::I2c as i32, ack));
        }
    }
}
