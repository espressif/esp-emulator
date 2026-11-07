//! A Texas Instruments TMP121/TMP123 temperature sensor, written as a virtual
//! part.
//!
//! A part is a WebAssembly module that models a component wired to the chip's
//! pins. This one sits on the chip's SPI bus. It has no data input: while its
//! chip select is low it clocks out one 16-bit word, most significant bit
//! first, whose bits 15..3 are the temperature in 13-bit two's complement at
//! 0.0625 °C a count, bit 2 is 0 and bits 1..0 are not driven. It follows
//! "TMP121, TMP123 1.5°C Accurate Digital Temperature Sensor with
//! SPI-Compatible Interface" (SBOS273C); README.md lists what it leaves out.
//!
//! `Tmp123` is the device and knows nothing about WebAssembly. The
//! `export_part!` line at the end makes it a part: the `esp-part` crate
//! exports every function of `proto/PART_ABI.md`, `part_on_bus` included, and
//! moves the bytes between the shared buffer and the device.

use esp_part::prelude::*;

/// The sensor's own names for its pins (the datasheet's pin table). The board
/// says which chip pins they are wired to, as
/// `{ pins: { CS: 7, SCK: 6, SO: 2 } }`.
const CS: &str = "CS";
const SCK: &str = "SCK";
const SO: &str = "SO";

/// The range the datasheet's Table 2 covers, -55 to 150 °C. A temperature
/// outside it reads as its nearest end rather than wrapping: wrapped, "too hot"
/// would read as "very cold".
const MIN_C: f32 = -55.0;
const MAX_C: f32 = 150.0;

struct Tmp123 {
    /// The chip pins the sensor's own pins are wired to. `None` until the
    /// board says, or when it left the pin unconnected.
    cs: Option<u32>,
    sck: Option<u32>,
    so: Option<u32>,
    /// The temperature around the sensor, in degrees Celsius.
    temp_c: f32,
}

impl Default for Tmp123 {
    fn default() -> Self {
        // Room temperature, so a fresh sensor reads plausibly.
        Self { cs: None, sck: None, so: None, temp_c: 25.0 }
    }
}

impl Device for Tmp123 {
    const NAME: &'static str = "tmp123";
    const ON_BUS: bool = true;

    /// Where the board wired the part.
    fn configure(&mut self, config: &PartConfig) {
        self.cs = config.pins.get(CS).copied();
        self.sck = config.pins.get(SCK).copied();
        self.so = config.pins.get(SO).copied();
    }

    /// The chip select this sensor is selected by, and the clock and data
    /// pads beside it for the emulator to check. The emulator works out the
    /// SPI bus and the chip-select slot from the chip select. A sensor the
    /// board left without a chip select still claims, with it left out, so
    /// the emulator refuses the claim and says so.
    fn claims(&self) -> Vec<Claim> {
        let spi = SpiClaim { cs_pin: self.cs, clk_pin: self.sck, miso_pin: self.so, ..Default::default() };
        vec![Claim { what: Some(claim::What::Spi(spi)) }]
    }

    /// Somebody changed the temperature around the sensor. The names are the
    /// part's own, and `set:temperature=<°C>` puts a value in one step.
    fn on_input(&mut self, name: &str) {
        match set_value(name) {
            Some(("temperature", value)) => self.temp_c = value as f32,
            Some(_) => {} // Not a value this part has.
            None => match name {
                "warmer" => self.temp_c += 1.0,
                "cooler" => self.temp_c -= 1.0,
                _ => {} // Not an interaction this part has.
            },
        }
        self.temp_c = self.temp_c.clamp(MIN_C, MAX_C);
    }

    /// The guest's SPI transfer to this part. A read gets the word, most
    /// significant byte first; past it nothing drives SO, and the master reads
    /// 0xFF. A transfer that reads nothing needs no answer: the sensor has no
    /// data input, so it ignores what was written.
    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        let Some(bus_request::Op::Spi(spi)) = &request.op else { return None };
        if spi.rx_len == 0 {
            return None;
        }
        let mut data = self.word().to_be_bytes().to_vec();
        data.resize(spi.rx_len as usize, 0xFF);
        Some(BusReply { correlation_id: request.correlation_id, ack: true, data, op: BusOp::Spi as i32 })
    }

    fn state(&self) -> Option<State> {
        Some(State::new().set("temperature", self.temp_c))
    }
}

impl Tmp123 {
    /// The word the sensor clocks out: the temperature in counts of 0.0625 °C,
    /// in bits 15..3, the three low bits 0.
    fn word(&self) -> u16 {
        let counts = (self.temp_c / 0.0625).round() as i16;
        (counts << 3) as u16
    }
}

// Big enough for any read the SPI driver makes of this sensor without DMA,
// which is at most 64 bytes, and for the write a host hands over with it. A
// host refuses a request larger than the buffer, and the part fails: a
// firmware writing kilobytes to this chip select is not talking to a TMP123.
esp_part::export_part!(Tmp123, buffer = 512);

// The device runs on the host too, so `cargo test` checks it without an
// emulator.
#[cfg(test)]
mod tests {
    use super::*;

    /// Wired as README.md's board wires it: CS on GPIO 7, SCK on 6, SO on 2.
    fn sensor() -> Tmp123 {
        let mut sensor = Tmp123::default();
        let pins = [(CS.to_string(), 7), (SCK.to_string(), 6), (SO.to_string(), 2)].into();
        sensor.configure(&PartConfig { pins, ..Default::default() });
        sensor
    }

    fn request(tx: &[u8], rx_len: u32) -> BusRequest {
        let spi = SpiTransfer { bus: 2, tx: tx.to_vec(), rx_len, ..Default::default() };
        BusRequest { correlation_id: 7, op: Some(bus_request::Op::Spi(spi)) }
    }

    /// The bytes a read of `rx_len` gets.
    fn read(sensor: &mut Tmp123, rx_len: u32) -> Vec<u8> {
        sensor.on_bus(&request(&[], rx_len)).expect("a read is answered").data
    }

    #[test]
    fn every_row_of_the_datasheets_table_2_reads_as_listed() {
        // SBOS273C Table 2, "Temperature Data Format".
        let table = [
            (150.0, 0x4B00),
            (125.0, 0x3E80),
            (25.0, 0x0C80),
            (0.0625, 0x0008),
            (0.0, 0x0000),
            (-0.0625, 0xFFF8),
            (-25.0, 0xF380),
            (-55.0, 0xE480),
        ];
        let mut sensor = sensor();
        for (c, word) in table {
            sensor.temp_c = c;
            assert_eq!(read(&mut sensor, 2), u16::to_be_bytes(word), "{c} °C");
            assert_eq!(sensor.word() & 0b111, 0, "{c} °C: the three low bits are 0");
        }
    }

    #[test]
    fn a_read_longer_than_the_word_reads_an_undriven_line_after_it() {
        let mut sensor = sensor();
        assert_eq!(read(&mut sensor, 4), [0x0C, 0x80, 0xFF, 0xFF]);
        assert_eq!(read(&mut sensor, 1), [0x0C], "the master decides how much it clocks");
    }

    #[test]
    fn a_write_is_not_answered() {
        // No data input: the transfer completed when it was sent.
        assert_eq!(sensor().on_bus(&request(&[0xAA], 0)), None);
    }

    #[test]
    fn the_reply_echoes_the_correlation_id_and_names_spi() {
        let reply = sensor().on_bus(&request(&[], 2)).unwrap();
        assert_eq!((reply.correlation_id, reply.op, reply.ack), (7, BusOp::Spi as i32, true));
    }

    #[test]
    fn it_claims_its_chip_select_with_the_clock_and_data_pads_beside_it() {
        let spi = SpiClaim { cs_pin: Some(7), clk_pin: Some(6), miso_pin: Some(2), ..Default::default() };
        assert_eq!(sensor().claims(), [Claim { what: Some(claim::What::Spi(spi)) }]);
        // Unwired: claimed without a chip select, which the handshake refuses.
        let none = SpiClaim::default();
        assert_eq!(Tmp123::default().claims(), [Claim { what: Some(claim::What::Spi(none)) }]);
    }

    #[test]
    fn the_inputs_move_the_reading_and_stop_at_the_tables_ends() {
        let mut sensor = sensor();
        sensor.on_input("warmer");
        sensor.on_input("wiggle");
        // 26 °C is 416 counts, 0x0D00.
        assert_eq!(read(&mut sensor, 2), [0x0D, 0x00]);
        for _ in 0..200 {
            sensor.on_input("warmer");
        }
        assert_eq!(read(&mut sensor, 2), [0x4B, 0x00], "150 °C");
        for _ in 0..300 {
            sensor.on_input("cooler");
        }
        assert_eq!(read(&mut sensor, 2), [0xE4, 0x80], "-55 °C");
    }

    #[test]
    fn a_set_puts_an_exact_value_and_stays_in_range() {
        let mut sensor = sensor();
        sensor.on_input("set:temperature=30.5");
        // 30.5 °C is 488 counts, 0x0F40.
        assert_eq!(read(&mut sensor, 2), [0x0F, 0x40]);
        sensor.on_input("set:temperature=-60");
        assert_eq!(read(&mut sensor, 2), [0xE4, 0x80], "clamped to -55 °C");
        for odd in ["set:temperature=1e2", "set:temperature=", "set:temperature=.5", "set:Temperature=5",
            "set:humidity=5", "set:temperature"] {
            sensor.on_input(odd);
        }
        assert_eq!(read(&mut sensor, 2), [0xE4, 0x80], "anything else is ignored");
    }

    #[test]
    fn it_shows_its_temperature() {
        let mut sensor = sensor();
        sensor.on_input("set:temperature=30.5");
        assert_eq!(sensor.state().unwrap().get("temperature"), Some("30.5"));
    }
}
