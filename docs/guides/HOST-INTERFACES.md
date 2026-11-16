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
value, or `err: ...`).

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
| `ping` | answers `ok` |
| `part list` | the `--cosim-board` parts, by name |
| `part NAME input KIND` | hands a part an interaction it understands, e.g. `press` |
| `part NAME input KIND after MS` | the same, MS of **emulated** time from now, so `press` then `release after 3000` is a 3 s hold on the device's clock however slowly the emulator runs |
| `part NAME input KIND at MS` | the same, at emulated time MS; name several steps against one `part clock` reading and their spacing holds. A time already past is handed over at once |
| `part NAME state` | `ok NAME=VALUE...`: what the part shows, in its own words (`pressed=false`, `temperature=22.5`), name-sorted, a space, newline or backslash in a name or value escaped as `\ `, `\n`, `\\`; `ok` alone for a part showing nothing; `err` for one without `part_state` |
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
`proto/esp_cosim.proto` on a socket, and each connection is one device.

A connected device does everything a part does. It registers with its claims:
the pins it drives, the pins it wants every transition of, the I2C address
or SPI chip select it answers, the I2S data pads it is the audio device
on, and the pads it is the camera sensor on. It then hears each watched pin at every transition and every other pin
once a tick, plus the heartbeat. It drives pins at a named emulated time,
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

`tools/cosim-tcp-press.py` is the whole thing in a few dozen lines of Python with
no protobuf library (the framing and the few messages it needs are
`tools/cosim_tcp.py`, beside it): it registers as the device on GPIO 9, waits
for a heartbeat, and presses the pin for 300 ms of emulated time.

```sh
esp-emu --chip esp32c3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5590 &
python3 tools/cosim-tcp-press.py 5590
```

`tools/cosim-tcp-reset.py` is a device on the chip's EN pin: it holds the
chip in reset for 200 ms of emulated time, or the time given, and reports the
line falling and rising and the reset that follows.

```sh
python3 tools/cosim-tcp-reset.py 5590 500
```

`tools/cosim-lcd-view.py` is a device that answers the bus: an SPI display
(the ST7789 / ILI9341 command set, which `esp_lcd` and the raw
`spi_master/lcd` example both speak) that claims the panel's chip select,
reads D/C from the pin levels each transfer carries, answers the ID read, and
draws the pixels into a PNG, rewritten as the firmware fills the screen:

```sh
esp-emu --chip esp32s3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5591 &
python3 tools/cosim-lcd-view.py 5591 --cs 10 --dc 9 --out lcd.png
```

A QSPI panel (CO5300, SH8601, ST77916) has no D/C line: leave `--dc` out, and
the script takes the command from the opcode word that opens each chip-select
frame.

A write completes as the firmware clocks it, so the panel never holds the
chip up; a read parks the firmware until the script answers. Together with
`--i2c-dev` for the board's expander and codecs, this is enough to watch a
product firmware's LVGL screen come up.

`tools/cosim-tcp-i2c.py` is an I2C board: a PCA9548A mux at 0x70, a PCF8575
expander at 0x20 holding the sensors' XSHUT lines, and a VL53L0X at 0x29
behind each of the eight mux channels. One script claims the three addresses
and routes 0x29 to the sensor on the channel the mux selects, so eight devices
share one address the way they do on the wire; a sensor whose XSHUT bit is
low NACKs, and `--absent N` leaves channel N's sensor off the board:

```sh
python3 tools/cosim-tcp-i2c.py 5593 --scl 7 --sda 6 &
esp-emu --chip esp32c3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5593
```

`tools/cosim-tcp-i2s.py` is an audio codec's sample path: a speaker on the
pad the chip transmits on, recording what the firmware plays to a WAV file,
and a microphone on the pad it receives on, playing it a 440 Hz tone. The
pads are the firmware's `dout` and `din`; name either or both:

```sh
esp-emu --chip esp32s31 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5594 &
python3 tools/cosim-tcp-i2s.py 5594 --dout 52 --din 40 --wav played.wav
```

An audio device is told each direction's sample rate, slots and width when
the firmware has programmed them (`i2s_format(payload)`), receives what the
unit transmits as it is clocked out (`i2s_out(payload)`), and supplies the
receiver with `i2s_in(pcm)` in the layout it was told, as emulated time
passes: the script keeps 50 ms queued ahead of the heartbeat's time. A
receiver left short takes silence after 20 ms rather than blocking the
firmware. The emulator puts no BCK, WS or MCLK on pads, so the format message
is all a device learns of the clock. A codec's control port is an I2C device
like any other, in the same registration. A `.wasm` part cannot claim I2S
yet; `--i2s-wav` and `--i2s-wav-in` do the same job from files, without a
device.

`tools/cosim-tcp-camera.py` is a DVP camera sensor on ESP32-S3, ESP32-P4 or
ESP32-S31: it answers the sensor's SCCB probe on the I2C pads and sends
frames to the chip's camera receiver, which the stock `esp_driver_cam` DVP
controller (and `esp_video` on top of it) then delivers. The pads are the
firmware's `esp_cam_ctlr_dvp_pin_config_t` and its SCCB bus:

```sh
esp-emu --chip esp32s31 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5594 &
python3 tools/cosim-tcp-camera.py 5594 --pclk 54 --vsync 56 --de 57 --xclk 55 \
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
acknowledged and read back, not interpreted, so give the script the format
and size the firmware asks its sensor for: a raw frame of another size is
refused, since the camera driver could not tell.

A sensor is on the receiver's bus once the firmware routes the receiver's
pixel clock to the claimed pad. It is told what the chip side looks like
(`dvp_format(payload)`: whether the receiver is capturing, the XCLK the
firmware routed out to it, the polarities) and sends each frame with
`cam_frame(data, width, height, fourcc, due_ns)`, in pieces the transport
takes. The emulator moves whole frames, not pixel-clock edges: nothing is
driven on the PCLK, VSYNC, DE or data pads. A frame sent while the receiver
is stopped is lost, as on the wire, and so is one sent for a capture session
that has since ended: the format message numbers the sessions, and a frame
carries the number its sensor last heard. The emulator is not held to the wall
clock, so the script keeps two frames queued ahead of emulated time; firmware
that counts frames per second sees the sensor's rate only from a device that
does the same. A `.wasm` part cannot claim a sensor yet, and there is no
command-line flag that feeds frames: a sensor is a device with a control bus,
a format the firmware programs and a cadence of its own, which is what a
consumer on this bus is.

`tools/cosim-tcp-dsi-view.py` is the panel on ESP32-P4's MIPI-DSI link: it
draws what the firmware shows, through the stock `esp_lcd` DSI bus, DBI
command channel and DPI panel, into a PNG:

```sh
esp-emu --chip esp32p4 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5595 &
python3 tools/cosim-tcp-dsi-view.py 5595 --out dsi.png
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
script draws the frame buffer; it does not apply the panel's own state (sleep,
display off, brightness). A read the firmware makes of the panel is answered
by the emulator with zeros. A `.wasm` part can claim a panel and is handed the
same messages (`proto/PART_ABI.md`), though it has no way yet to show the
picture. There is no command-line flag that writes frames: the script is that.

Start a script before the emulator when the firmware talks to the device at
boot: the emulator does not wait for a consumer, and `connect` retries for a
few seconds. An I2C device of your own is the same loop on the helpers in
`cosim_tcp.py`: `i2c_claim(addr, scl, sda)` in a `register` (several claims
make one consumer a board), `i2c_request(payload)` for each transaction as
the firmware clocked it (its write bytes, the length it reads back, and
whether it continues a read the controller split), and `i2c_reply(id, data)`
or `i2c_reply(id, ack=False)` for a NACK.

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
