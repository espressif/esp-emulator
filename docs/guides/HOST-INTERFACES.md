# Host Interfaces, Tracing and Debugging

The channels between the host and the running firmware: scripted UART input, a TCP bridge for UART1, the control channel that stands in for DTR/RTS resets and flash operations, and the per-task execution trace. Flashing over `socket://` has its own page: [FLASHING.md](FLASHING.md).

## UART Injection

The `--inject` and `--inject-on` flags work in pairs to send data to the firmware's UART when specific output is detected:

```sh
esp-emu \
  --firmware app.bin \
  --inject "yes\n" --inject-on "Continue? [y/n]"
```

Standard input is also forwarded to UART RX line-by-line.

## UART1 TCP Bridge

`--uart1-tcp HOST:PORT` exposes UART1 as a TCP server, independent of the
UART0 console. Use it to simulate an external serial device (GPS module,
sensor, modem) with a host-side script:

```sh
esp-emu --chip esp32c3 \
  --firmware build/merged_flash.bin \
  --uart1-tcp 127.0.0.1:5556
```

```py
# Host-side mock device: talk to the firmware's UART1
import socket
s = socket.create_connection(("127.0.0.1", 5556))
s.sendall(b"$GPGGA,123519,4807.038,N\n")   # firmware sees it on UART1 RX
print(s.recv(4096))                         # firmware's UART1 TX
```

Works on all supported chips (verified with ESP-IDF's `uart_echo` example on
C3 and P4). One client at a time; the listener stays up across reconnects.
When no client is connected, firmware TX on UART1 is discarded — same as
QEMU's unattached chardev.

## Host Control Channel

`--control-tcp HOST:PORT` serves the device-state operations a UART socket
cannot express. On hardware a reset is a DTR/RTS toggle into EN; pyserial's
`socket://` transport drops modem-control lines, so `dut.serial.hard_reset()`
and friends have no in-band form. The channel fills the role QEMU gives QMP,
without the protocol: newline-delimited commands, one reply per line (`ok`, a
value, or `err: ...`). An empty line gets no reply.

| command | effect |
|---|---|
| `reset` | chip reset (what a DTR/RTS toggle does on a board) |
| `reset --soft` | software reset |
| `en low` | pulls the chip's EN (CHIP_PU) pin low: the chip is held in reset and runs nothing, while emulated time and the board's parts go on |
| `en high` | lets EN go: the chip boots as from power-on (`esp_reset_reason()` is `ESP_RST_POWERON`), once no part holds EN low either |
| `erase-flash` | as `esptool erase-flash`, on the running machine's flash |
| `erase-region OFFSET LENGTH` | as `esptool erase-region`; numbers in hex or decimal |
| `write-region OFFSET FILE` | as `esptool write-flash`, reading the data from a path |
| `read-region OFFSET LENGTH FILE` | as `esptool read-flash`, writing the data to a path |
| `efuse-load FILE` | reads a QEMU-compatible eFuse image into the running machine and resets it, so bits a download-mode instance burned take effect without restarting the process |
| `ping` | answers `ok` |
| `help` | `ok`, then every command with its arguments, separated by ` \| `, on one line |
| `part list` | the `--cosim-board` parts, by name |
| `part NAME input KIND` | hands a part an interaction it understands, e.g. `press` |
| `part NAME input KIND after MS` | the same, MS of **emulated** time from now, so `press` then `release after 3000` is a 3 s hold on the device's clock however slowly the emulator runs |
| `part NAME input KIND at MS` | the same, at emulated time MS; name several steps against one `part clock` reading and their spacing holds. A time already past is handed over at once |
| `part NAME state` | `ok NAME=VALUE...`: what the part shows, in its own words (`pressed=false`, `temperature=22.5`), name-sorted, a space, newline or backslash in a name or value escaped as `\ `, `\n`, `\\`; `ok` alone for a part showing nothing; `err` for one without `part_state` |
| `part NAME render` | `ok HEX`: what the part draws that is new since it was last asked, the bytes its `part_render` wrote, in the layout the part states; `ok` alone when nothing is new; `err` for a part without `part_render` |
| `part NAME video-format` | `ok WIDTH HEIGHT FOURCC FPS`: what a part that is shown pictures asks for, from its `part_video_format`; `ok` alone when it wants nothing now; `err` for a part without it |
| `part NAME video FILE` | `ok`: shows the part the picture in FILE (`part_video_in`), whole, at the size it asked for: RGB888, or a JPEG for a JPEG mode; `err:` and why not when it does not fit the part's buffer or the part refuses it. The rest of the line is the path |
| `part pending` | `ok N`: scheduled inputs not yet handed over |
| `part clock` | `ok MS`: emulated time |

```sh
esp-emu --chip esp32c3 --firmware build/merged_flash.bin \
  --uart-tcp 127.0.0.1:5555 --control-tcp 127.0.0.1:5556

printf 'erase-region 0x9000 0x6000\nreset\n' | nc -N 127.0.0.1 5556   # wipe NVS, reboot
```

The emulator answers each command in order, then closes the connection once
the client has closed its sending side, so `nc` exits after the last answer.
`-N` makes Linux's `nc` close that side at the end of its input; macOS's `nc`
does it unasked and reads `-N` as another option, so leave `-N` out there.

The emulator process keeps running across a reset, so a harness such as
pytest-embedded keeps one continuous output stream, expect history and log.
Flash operations act on the flash the running firmware sees; rewriting the image
file from outside would not, since the flash contents are carried across resets
in memory.

## Co-simulation over TCP

`--cosim-tcp HOST:PORT` lets a device model that is not a `.wasm` part join
the board: a Python script standing in for a sensor, another simulator, a
test harness in any language. It serves the co-simulation bus of
`proto/esp_cosim.proto` on a socket, and each connection is one device, or a
board of several registered together.

A connected device does everything a part does. It registers with its claims:
the pins it drives, the pins it wants every transition of, the I2C address
or SPI chip select it answers, the I2S data pads it is the audio device
on, and the pads it is the camera sensor on. It then hears each watched pin at every transition and every other pin
once a tick, plus the heartbeat. Pin levels go to a device whose registration
has a `GpioClaim`, even an empty one, and to one that registers with no claim
at all and only watches. A device with claims of other kinds only (a clock
holder, an I2C or SPI device) is sent none, so pads the firmware toggles are
not sent to it; it adds a `GpioClaim` to hear them. A connection hears no
broadcast before its registration is accepted. It drives pins at a named emulated time,
answers bus transactions, exchanges audio with an I2S unit, and sends frames
to the camera receiver. One that claims the chip's EN pin holds the chip in
reset and lets it go, on emulated time. When it
disconnects its claims are released, as for a device unplugged, and a level
it drove stays on the pin until something else drives it; EN it lets go.

`--cosim-tcp` goes with `--cosim-board`: the parts and the connections share
one chip, and a pin one of them claims is refused to the other. The board's
parts make their claims before the first connection is taken, so a client
cannot take a pin from under a part.

Both directions carry length-prefixed frames, `[len: u32 LE][frame]`. From
the emulator, a frame is `[topic_len: u16 LE][topic][EmulatorMessage]`: a
broadcast carries its topic (`SIM/CLOCK`, `SIM/RESET`, `SIM/EN`, `HW/GPIO/4/LEVEL`), a
reply to this consumer alone carries `@` and its name (`@tcp:1`). To the
emulator, a frame is a bare `EmulatorMessage`. A consumer that stops reading
loses broadcasts rather than holding the chip; a frame over 256 KiB closes the
connection.

Nothing authenticates a connection, as nothing does on `--uart-tcp` or
`--control-tcp`: whoever can reach the port is a consumer. Bind loopback
unless the network is trusted.

`tools/cosim-tcp.py` runs a device on the socket, in Python with no protobuf
library. Its devices are classes in `tools/cosim_tcp/devices/`; run it with no
arguments for the list, and `PORT DEVICE --help` for one device's options.
Several devices joined by `+` share the connection, as one board:
`cosim-tcp.py 5590 press --pin 9 + lcd-view --cs 10 --dc 8`.

`press` registers as the device on a GPIO (9 unless `--pin` names another),
waits for a heartbeat, and presses the pin for 300 ms of emulated time.

```sh
esp-emu --chip esp32c3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5590 &
python3 tools/cosim-tcp.py 5590 press
```

`reset` is a device on the chip's EN pin: it holds the
chip in reset for 200 ms of emulated time, or the time given, and reports the
line falling and rising and the reset that follows.

```sh
python3 tools/cosim-tcp.py 5590 reset --ms 500
```

`lcd-view` is a device that answers the bus: an SPI display
(the ST7789 / ILI9341 command set, which `esp_lcd` and the raw
`spi_master/lcd` example both speak) that claims the panel's chip select,
reads D/C from the pin levels each transfer carries, answers the ID read, and
draws the pixels into a PNG, rewritten as the firmware fills the screen:

```sh
esp-emu --chip esp32s3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5591 &
python3 tools/cosim-tcp.py 5591 lcd-view --cs 10 --dc 9 --out lcd.png
```

A QSPI panel (CO5300, SH8601, ST77916) has no D/C line: leave `--dc` out, and
the device takes the command from the opcode word that opens each chip-select
frame.

A write completes as the firmware clocks it, so the panel never holds the
chip up; a read parks the firmware until the device answers. Together with
`--i2c-dev` for the board's expander and codecs, this is enough to watch a
product firmware's LVGL screen come up.

`spi-master` is the other way round: the consumer is the SPI master and the
chip its slave, as on a board where an ESP32 is the Wi-Fi co-processor of a
host MCU. The firmware runs IDF's stock `spi_slave` driver; the device claims
the chip select that driver listens on (`spi_master_claim(cs_pin)`) and
watches the handshake pad. Each time the firmware raises it, the device
clocks one transfer and prints what the slave sent back, which is what IDF's
`examples/peripherals/spi_slave/sender` does on a second board:

```sh
esp-emu --chip esp32c6 --firmware receiver/build/merged_flash.bin --cosim-tcp 127.0.0.1:5592 &
python3 tools/cosim-tcp.py 5592 spi-master --cs 14 --handshake 2
```

A transfer is one message each way. `spi_master_transfer(at_ns, ident, tx,
sck_hz)` drops chip select at `at_ns` of emulated time and clocks `tx` in at
`sck_hz`; the firmware's transaction completes when chip select rises,
`8 * len(tx) / sck_hz` later. The `SpiMasterResult` that answers it carries
as many bytes back, and is sent at once, stamped with the time chip select
rises. `selected` false means the pad selects no slave: the firmware has not
started its driver, or the chip reset. The emulator never stops for a master,
and a slave with no transaction loaded is clocked all the same, so a
protocol needs the handshake line a board would have; handshake, data-ready
and reset lines are ordinary pins (`gpio_claim`, `en_claim`).

A master that does not know its whole transfer in advance sends it in pieces
with `hold_cs=True`: chip select stays low, each piece is answered with its
own bytes, and the firmware's transaction completes with the piece that
leaves `hold_cs` off, which may be empty. That is how another simulator is
the master. Its SPI controller model hands a device one byte at a time and
wants that byte's answer before the next (Renode's `ISPIPeripheral.Transmit`
does), so each byte is a piece and the chip-select line rising is the empty
closing one.

What is modelled is the full-duplex slave on one data line each way, in whole
bytes, with or without DMA, on every chip. Not modelled: the half-duplex
slave (`spi_slave_hd`), SCK and chip-select edges on their pads (a GPIO
interrupt on the chip-select pad does not fire unless the consumer also
drives the pad), and the SPI mode, which is not compared with the master's.
The emulator and a second simulator each keep their own clock, unless the
other one holds the emulator's (below). `tools/renode/` has the master's end
for Renode: a device for a Renode machine's SPI controller that is this chip
(`tools/renode/README.md`).

`clock` holds the chip's clock. By default the emulator runs at its own pace
and waits for no consumer; one that registers with `time_claim()` changes
that: the chip then runs only as far as that consumer's `time_grant(advance_ns)`
messages reach, stops, and says so with a `TimeWaiting` (`time_waiting(payload)`
gives the emulated time it stands at). Grants add up from where the last one
ended. With several holders the chip runs as far as the one that granted
least, and only that one is told it is waited on. While it stands still the socket is still served: a pin driven or an
SPI transfer that arrives is taken at that instant and answered. The device
grants one quantum each time the chip has used the last, paced against the
host's clock if asked:

```sh
esp-emu --chip esp32c3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5596 &
python3 tools/cosim-tcp.py 5596 clock --rate 1 --quantum-ms 1     # real time
python3 tools/cosim-tcp.py 5596 clock --rate 0.5                  # half speed
```

Two emulators each held at the same rate advance together. A simulator with
a clock of its own grants a quantum each time its clock has advanced by one,
after waiting for the `TimeWaiting`: the two are then never more than a
quantum apart. The emulator's own pacing against the host's clock is off
while a consumer holds the clock, awake or asleep. What a network backend
brings from the host still arrives in host time, so with one attached a run
is bounded by its grants but not repeatable. Run `clock` on a connection of
its own, not joined to other devices with `+`: a paced clock sleeps between
grants. `--for-ms N` stops granting after N ms of emulated time.

`i2c-board` is an I2C board: a PCA9548A mux at 0x70, a PCF8575
expander at 0x20 holding the sensors' XSHUT lines, and a VL53L0X at 0x29
behind each of the eight mux channels. One device claims the three addresses
and routes 0x29 to the sensor on the channel the mux selects, so eight devices
share one address the way they do on the wire; a sensor whose XSHUT bit is
low NACKs, and `--absent N` leaves channel N's sensor off the board:

```sh
python3 tools/cosim-tcp.py 5593 i2c-board --scl 7 --sda 6 &
esp-emu --chip esp32c3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5593
```

`i2s` is an audio codec's sample path: a speaker on the
pad the chip transmits on, recording what the firmware plays to a WAV file,
and a microphone on the pad it receives on, playing it a 440 Hz tone. The
pads are the firmware's `dout` and `din`; name either or both:

```sh
esp-emu --chip esp32s31 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5594 &
python3 tools/cosim-tcp.py 5594 i2s --dout 52 --din 40 --wav played.wav
```

An audio device is told each direction's sample rate, slots and width when
the firmware has programmed them (`i2s_format(payload)`), receives what the
unit transmits as it is clocked out (`i2s_out(payload)`), and supplies the
receiver with `i2s_in(pcm)` in the layout it was told, as emulated time
passes: the device keeps 50 ms queued ahead of the heartbeat's time. A
receiver left short takes silence after 20 ms rather than blocking the
firmware. The format message is all a device needs of the clock: BCK and MCLK
are on no pad, and a master direction's WS is on its pad, every edge for a
consumer that watches it. A codec's control port is an I2C device like any other, in the
same registration. A `.wasm` part is the same audio device inside the
emulator (`examples/parts/inmp441`, `examples/parts/max98357a`), and
`--i2s-wav` and `--i2s-wav-in` do the same job from files, without a
device.

`camera` is a DVP camera sensor on ESP32-S3, ESP32-P4 or
ESP32-S31: it answers the sensor's SCCB probe on the I2C pads and sends
frames to the chip's camera receiver, which the stock `esp_driver_cam` DVP
controller (and `esp_video` on top of it) then delivers. The pads are the
firmware's `esp_cam_ctlr_dvp_pin_config_t` and its SCCB bus:

```sh
esp-emu --chip esp32s31 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5594 &
python3 tools/cosim-tcp.py 5594 camera --pclk 54 --vsync 56 --de 57 --xclk 55 \
    --data 46,47,48,49,50,51,52,53 --scl 1 --sda 0 --sensor ov2640 \
    --fourcc YUYV --size 640x480 --fps 6
```

Those are the pads and the format of IDF's own `esp_driver_cam` test on
ESP32-S31 (`test_apps/dvp`, its OV2640 case), which passes against it.

`--frames` is a JPEG file or a directory of them, sent in name order and
repeated; without it the sensor sends moving colour bars in `--fourcc` at
`--size`, at `--fps` frames a second of emulated time. `--sensor` picks whose
identity registers answer the driver's probe (OV2640, OV3640, OV5640; `--reg`
and `--addr` for another). What the firmware then programs over SCCB is
acknowledged and read back, not interpreted, so give the device the format
and size the firmware asks its sensor for: a raw frame of another size is
refused, since the camera driver could not tell.

A sensor is on the receiver's bus once the firmware routes the receiver's
pixel clock to the claimed pad. It is told what the chip side looks like
(`dvp_format(payload)`: whether the receiver is capturing, the XCLK the
firmware routed out to it, the polarities) and sends each frame with
`cam_frame(cam_frame_pieces(data, width, height, fourcc), due_ns)`, in pieces
the transport takes. The emulator moves whole frames, not pixel-clock edges: nothing is
driven on the PCLK, DE or data pads. VSYNC is pulsed on the pad the firmware
routes to the receiver's VSYNC input as each frame starts, and once a frame
period while the receiver is stopped, for a driver such as `esp_video`'s that
starts the receiver from that pad's interrupt. A frame sent while the receiver
is stopped is lost, as on the wire, and so is one sent for a capture session
that has since ended: the format message numbers the sessions, and a frame
carries the number its sensor last heard. The emulator is not held to the wall
clock, so the device keeps two frames queued ahead of emulated time; firmware
that counts frames per second sees the sensor's rate only from a device that
does the same. A `.wasm` part can be the sensor too, through the same
messages (`examples/parts/ov2640`, [PARTS.md](PARTS.md#wire-a-camera-part)).
A sensor with a control bus, a format the firmware programs and a cadence of
its own is a consumer on this bus, or a part. Frames from files need none of that: `--camera-frames`
feeds them in a format it is told, and `--camera-mjpeg` or `--camera-png`
writes what the firmware received (see [CLI.md](CLI.md)). The two do not mix:
a receiver `--camera-frames` (or a page) feeds stays theirs, and a consumer
on its pixel clock is told so, once, and never put on the bus.

`csi-camera` is a MIPI CSI camera sensor on ESP32-P4: it answers the
sensor's SCCB probe on the I2C pads and sends Bayer frames to the CSI host,
which the stock `esp_driver_cam` CSI controller delivers, through the ISP or
past it. The lanes are the chip's dedicated D-PHY pads, so only the SCCB
pads are named:

```sh
esp-emu --chip esp32p4 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5594 &
python3 tools/cosim-tcp.py 5594 csi-camera --scl 8 --sda 7 --sensor ov5647 \
    --size 800x640 --order BGGR --depth 8 --fps 50
```

Those are the pads and the mode of IDF's own `esp_driver_cam` CSI test
(`test_apps/csi`, its OV5647 case), which passes against it. The frames are
moving colour bars as a mosaic in `--order` at `--depth` bits (8 or 10) and
`--size`; give the shape the firmware programs the ISP (or, with the ISP
off, the bridge) for, since a frame of another shape is dropped and counted
as the bridge's `DISCARD`. `--sensor` picks whose chip ID answers the probe
(SC2336, OV2710, OV5647, SC202CS).

The sensor is on the link from the moment it registers with its `CsiClaim`
(`csi_claim()`): there are no pads to route. It is told what the chip side
looks like (`csi_format(payload)`: whether the host is capturing, the lanes
it listens on, and the session) and sends each frame with
`cam_frame(cam_frame_pieces(data, width, height, fourcc), due_ns, session)`: `BA81`, `GBRG`, `GRBG` or
`RGGB` at 8 bits a sample, `BG10`, `GB10`, `BA10` or `RG10` at 10, one
little-endian u16 a sample. While a sensor is attached the CSI host's test
pattern stops, and its frames arrive at the times it stamps them. Without a
sensor, `--csi-sensor` answers the probe and the test pattern runs. A `.wasm`
part can be the CSI sensor too (`examples/parts/ov5647`).

`usb-host` is the computer a USB device firmware is plugged into, on a
USB-OTG port of ESP32-S3, ESP32-P4 or ESP32-S31. Without it the emulator's
own host enumerates the device and serves a CDC-ACM console; with it that
host is off the port, and the consumer enumerates the device and runs every
transfer, so a mass-storage or HID firmware has something at the other end
of the cable:

```sh
esp-emu --chip esp32p4 --firmware tusb_msc/build/merged_flash.bin --cosim-tcp 127.0.0.1:5596 &
python3 tools/cosim-tcp.py 5596 usb-host --msc-dump disk.img
```

It prints the device's descriptors and then plays the host of each interface
it knows: a mass-storage disk is asked INQUIRY and READ CAPACITY and has its
first block read (`--msc-write FILE` writes a file onto it, `--msc-dump FILE`
reads it whole), a HID interface has its reports printed (`--reports N`
leaves after N; add `+ press --pin N` for a button the firmware reads), and a
CDC-ACM port is opened and what the device writes is printed. IDF's
`tusb_msc` and `tusb_hid` examples run against it unchanged.

The claim names a port, not pads (`usb_host_claim(port)`, 0 unless the chip
has two: ESP32-P4's high-speed port is 0, its full-speed port 1). The
emulator keeps the electrical end: when the firmware turns its pull-up on,
the bus is reset and `usb_link(payload)` says the device is there and at
what speed. Everything else is a transfer: `usb_transfer(at_ns, ident,
endpoint, setup, data, length, timeout_us)` is a control transfer when it
carries an 8-byte `setup`, else `data` out or up to `length` bytes in, and
the `UsbResult` that answers it (`usb_result(payload)`) carries a status
(`ok`, `stall`, `timeout`, `no-device`, `overflow`, `refused`), what the
device sent and how much it took. The emulator never stops for a host: a
transfer waits while the endpoint is not armed, up to its timeout, and
transfers on different endpoints run side by side, so an interrupt IN may be
left waiting for a report while control and bulk transfers go on. A device
the emulator's own host had configured is reset when a consumer takes the
port, and again when it leaves: the firmware is enumerated twice, and one
that sends on its own while the bus is reset, as IDF's `tusb_hid` does for
two seconds after it is mounted, may be caught mid-transfer. `--usb-host
cosim` keeps the emulator's own host off the ports: a port is a cable in
nothing until a consumer claims it, and again once it leaves, so the
consumer's enumeration is the device's only one. The device is not told
when its consumer leaves (VBUS going away is not modelled): it stays
configured, and a transfer it has armed waits. The emulator's own host
serves the CDC-ACM console, so `--console usb-cdc` is refused with it. Packets, data toggles, suspend and a reset
the host asks for are not modelled, and isochronous endpoints have no
timing. A `.wasm` part can be the host too.

`dsi-view` is the panel on ESP32-P4's MIPI-DSI link: it
draws what the firmware shows, through the stock `esp_lcd` DSI bus, DBI
command channel and DPI panel, into a PNG:

```sh
esp-emu --chip esp32p4 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5595 &
python3 tools/cosim-tcp.py 5595 dsi-view --out dsi.png
```

A panel claims a virtual channel of the link (`dsi_claim(vc)`, 0 unless the
firmware says otherwise), not pads: the DSI lanes are not on the GPIO matrix.
Each `dsi_event(payload)` is then one of three things. A command is a packet
the firmware sent the panel through the generic interface, as sent (`--commands`
prints them); a panel that attaches late is first sent the ones it missed. The
format is the video stream's size and pixel layout, RGB565 little-endian or
three bytes blue-green-red, sent when it changes. Rows are the rows of a frame
that differ from the last one the panel was sent, in the frame buffer's own
bytes: a still screen sends nothing, and the first frame comes whole. The
device draws the frame buffer; it does not apply the panel's own state (sleep,
display off, brightness). A read the firmware makes of the panel is answered
by the emulator with zeros. A `.wasm` part can claim a panel and is handed the
same messages (`proto/PART_ABI.md`) and shows the picture through
`part_render` (`part NAME render`, or a page's `render` message). There is no command-line flag that writes frames: the device is that.

Start a device before the emulator when the firmware talks to it at boot: the
emulator does not wait for a consumer, and the tool retries the connection for
a few seconds.

A device of your own is a class, not a new script. Put a file in
`tools/cosim_tcp/devices/` with a `Device` subclass that has a `name`; it is on
the command line under that name at once:

```python
from .. import wire
from ..device import Device

class Sht31(Device):
    """A temperature sensor at 0x44."""
    name = "sht31"

    @staticmethod
    def arguments(ap):
        ap.add_argument("--scl", type=int, default=7)
        ap.add_argument("--sda", type=int, default=6)

    def claims(self):
        return [wire.i2c_claim(0x44, self.args.scl, self.args.sda)]

    def i2c(self, t):                    # one transaction, as the firmware clocked it
        return True, bytes(t.read_len)   # (ack, the bytes it reads back)
```

`Device` (`tools/cosim_tcp/device.py`) has one method per event, each doing
nothing until overridden: `start` and `tick` for emulated time, `gpio`, `i2c`,
`spi`, `i2s_format` and `i2s_out`, `dvp_format`, `csi_format`, `dsi`, `en`,
`edges` (a run of edges of a pin claimed in `edge_pins`), `attached` (the
emulator's answer to the registration), `spi_result` (what a transfer the
device clocked as SPI master read back), `usb_link` and `usb_result` (the
device on a USB port it is the host of, and how a transfer ended), `time_waiting` (a clock holder's
grants ran out), `idle` (after every frame, for work paced by the host's
clock), `reset`, and `closed`. `Session` connects, registers every device's claims as one consumer,
hands an I2C transaction to the device that claimed its address and an SPI
transfer to the one that claimed a chip select, and sends the answer. `self.bus.send(...)` sends a message built by
`wire` (`gpio_drive`, `pulse_drive` for a whole waveform on a pin, `i2s_in`,
`cam_frame`), `self.bus.now` is emulated time,
and `self.finish()` says a one-shot device is done. A display that draws into
a PNG also inherits `Picture` (`picture.py`). The functions under all of it
(`connect`, `register`, the claims, `decode`) are importable as before with
`from cosim_tcp import ...`, for a script that wants its own loop.

## Execution Tracing and Profiling

`--trace FILE` writes a per-task execution trace in Chrome Trace Event format.
It needs `--elf`: the FreeRTOS `pxCurrentTCBs` symbol is what makes context
switches visible.

```sh
esp-emu --chip esp32c6 --firmware build/merged.bin \
        --elf build/app.elf --trace run.json

# Timeline: drag run.json into https://ui.perfetto.dev (or chrome://tracing)
# Summary tables on localhost:
python3 tools/trace-view.py run.json 8081
```

The timeline gives one swimlane per task, so a context switch is a slice
boundary. The local page covers what a timeline is bad at — CPU time per task and
the hot-function table from sampled PCs:

```
TASK                    CPU%   TIME(ms)  SLICES
IDLE                  77.69%    4111.13     261
main                  20.67%    1093.71      89
wifi                   1.52%      80.41      54
```

Unlike SEGGER SystemView or ESP-IDF's `app_trace`, **nothing is instrumented in
the guest**: there is no Kconfig to enable, no rebuild, and no CPU or RAM cost
inside the firmware, because the emulator reads the scheduler state directly.
The trade is resolution — sampling happens on the 256-cycle peripheral tick, so
slice boundaries are accurate to ~1.6 µs at 160 MHz and a task that runs for
less than one tick can be missed. It answers "where did the time go", not
"exactly when did this switch happen".

`--trace-pc-period N` sets the PC sampling rate in ticks (default 16, `0`
disables). A trace from a run that was killed or timed out still loads — the
format permits an unterminated array.
