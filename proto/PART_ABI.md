# The part ABI

A "part" is a model of a real component: a button, an LED, a sensor. It runs
outside the emulator and acts on the chip's pins, and answers on its I2C or SPI
bus, the way the real component would.

New to parts? [`docs/guides/PARTS.md`](../docs/guides/PARTS.md) is the
introduction: running the examples, wiring a board, and writing a part in
Rust with `esp-part`. This file is the contract itself, for a part written in
any language.

`esp_cosim.proto` says **what** the two sides send each other. This file says
**how** a part gets called when it is compiled to WebAssembly and loaded by a
host — `part-worker.js` in the browser build (`www/` in the release's wasm
tarball), or the native emulator itself.

Both use the same messages. A part drives a pin with a `GpioDrive` whether those
bytes cross a network socket or are written straight into its memory, so a part
does not need to know how it is being run.

## What a part exports

These are what a host calls. The module may import nothing. It may export
more — a Rust `cdylib` does — and a host ignores what it does not know.

```wat
(func (export "part_abi_version") (result i32))
(func (export "part_buffer")      (result i32))
(func (export "part_buffer_len")  (result i32))

(func (export "part_configure") (param i32))
(func (export "part_register")  (result i32))
(func (export "part_on_pins")   (param i64 i64))
(func (export "part_on_bus")    (param i32) (result i32))  ;; only if you claim an I2C address or an SPI chip select
(func (export "part_on_message") (param i32))        ;; only if you are sent one-way messages (a serial line, a PWM pin, edges, I2S, a camera, a USB port)
(func (export "part_poll")      (param i64) (result i32))

(func (export "part_on_input")  (param i32))
(func (export "part_state")     (result i32))        ;; optional: what the part shows
(func (export "part_render")    (result i32))        ;; optional: what the part draws
(func (export "part_audio_out") (result i32))        ;; optional: the sound the part makes
(func (export "part_audio_in")  (param i32))         ;; optional: a sound the part hears
(func (export "part_audio_rate") (param i32) (result i32))  ;; with either: each one's rate
(func (export "part_video_format") (result i32))     ;; optional: what the part asks to be shown
(func (export "part_video_in")  (param i32) (result i32)) ;; optional: a picture it is shown
(memory (export "memory") 1)
```

The first three are plumbing. The next six are the emulator's side. The last
eight are the *person's* side — somebody clicking on the part, somebody
looking at it, something drawing it, somebody hearing it or making a sound
at it, and something showing it a picture. The emulator has no idea a person
exists, so none of them reaches it.

All but `part_on_bus`, `part_on_message`, `part_state`, `part_render`, the
three `part_audio_*`, `part_video_format` and `part_video_in` are required. A
host checks for every
one when it loads a module, so a missing export is refused by name rather than
failing as a `TypeError` at its first call. `part_video_format` and
`part_video_in` come as a pair: a host refuses a module with one and not the
other.

`part_on_bus` is needed only by a part that claims an I2C address or an SPI
chip select, and a part without it has no bus. A host refuses a part that claims
one but does not export it, at load, for the same reason: the first transaction
to it would otherwise wait for an answer that cannot come.

`part_on_message` is needed only by a part that claims something the emulator
sends one-way messages about: a serial line, a pin it reads as PWM
(`GpioClaim.pwm_pins`) or as edges (`GpioClaim.edge_pins`), an I2S data pad, a DSI panel (`DsiClaim`), a camera
sensor's bus (`DvpClaim`, `CsiClaim`), or a USB port it is the host of
(`UsbHostClaim`). A part that claims one but does not
export it is refused the same way, since what the chip transmits to it, or the
waveform on its pin, would otherwise go nowhere. A microphone that only
supplies samples needs it too: the format it is to supply arrives that way, as
a camera sensor learns whether the receiver captures.

Three kinds of call carry the emulator's side, and a new channel fits one of
them rather than adding an export: a pin moved (`part_on_pins`), a question
the chip waits on (`part_on_bus`), or a one-way message the part is told and
answers, if at all, from `part_poll` (`part_on_message`). A new one-way
channel is a new `EmulatorMessage` kind, handed over through the same export.
The part's side is one call: everything a part sends, a `GpioDrive`, a
`UartRx`, an `AnalogDrive` or `AnalogWave`, an `I2sIn`, a camera's
`CamFrame` or a USB host's `UsbTransfer`, comes out of `part_poll`, so a channel the part drives adds a
message kind and no export.

### The buffer

There is one shared buffer, and the part owns it. The host writes the question
there, calls the function, and reads the answer back from the same place.

The part picks the size. A button needs a few dozen bytes. A part on a bus sizes
it for the longest transaction it expects: a display's frame is one write of a
kilobyte or more, and an SPI transfer through DMA can be 32 KB. A host never
writes more than `part_buffer_len()`: a message that does not fit is not
handed over, never truncated. A `PartConfig` that does not fit leaves the part
unregistered, with the reason reported; a bus request that does not fit is one
the part cannot answer, which fails the part (see [When a call
fails](#when-a-call-fails)), since the emulator is stopped waiting on it.
A part on a serial line is sent what the chip transmits at most 128 bytes at a
time, in a message of up to 147 bytes; a buffer that cannot hold one fails the
part at the first that does not fit. A part on an I2S data-out pad is sent
samples at most 4096 bytes at a time, in a message of up to 4114 bytes, on the
same terms. A part that supplies samples sizes its buffer for what it sends in
one poll (see `part_on_message`). A camera sensor is handed only formats,
`DvpFormat` and `CsiFormat`, a few dozen bytes with their envelope at most,
and a buffer that cannot hold one fails the part at the first; its frames go
the other way, in pieces it cuts to its own buffer (see `DvpFormat`).

There is no `malloc` in this contract. That is on purpose, so a part can be
written in a language with no runtime.

### Order of calls

```text
load        part_abi_version                     once; another version is refused
wire        part_configure  ->  part_register    once each, in this order
handshake   part_on_pins(levels at accept)       once, if the emulator accepts
run         part_on_pins                         after each pin change
            part_on_bus                          on each transaction to an address
                                                 or chip select this part claims
            part_on_message  ->  part_poll       on each one-way message addressed
                                                 to this part: what the chip
                                                 transmits on its serial line,
                                                 a change of the waveform on a
                                                 pin it reads as PWM, the
                                                 format and samples of its I2S
                                                 data pads, a run of edges of
                                                 a pin it reads as edges, the
                                                 format of its camera receiver,
                                                 the device on its USB port and
                                                 how each transfer ended
            part_poll                            after each pin change and heartbeat
reset       part_on_pins(levels after reset)     on each chip reset, then part_poll
person      part_on_input  ->  part_poll         on each interaction (part_poll
                                                 once the handshake is done)
            part_state                           whenever somebody looks
            part_render                          whenever something draws the part
            part_audio_out                       whenever somebody listens
            part_audio_in                        whenever somebody makes a sound at it
            part_audio_rate                      whenever a host needs a rate
            part_video_format -> part_video_in   whenever something shows the part
                                                 a picture
```

`part_buffer` and `part_buffer_len` are asked before every call that passes or
returns bytes.

## Reference

Integers are WebAssembly `i32` and `i64`. A host passes 64-bit values as
unsigned numbers (a `BigInt` in JavaScript); read them as `u64`.

### `part_abi_version() -> i32`

The ABI version this module implements: `PartAbiVersion.PART_ABI_VERSION_CURRENT`
from `esp_cosim.proto`, today `1`. Called once, when the host loads the module.
A host that does not implement the version refuses the module.

### `part_buffer() -> i32`

Where the shared buffer starts, as an offset into the exported `memory`. The
host asks before every use, so the answer may change between calls, but not
during one. A part may grow its memory to make the buffer, here or in any
other call, which in a browser detaches a view of the memory taken before: a
host takes its view of `memory` after this call, never before it.

### `part_buffer_len() -> i32`

How many bytes the shared buffer holds. The host never writes more, and it
treats a return value larger than this as `0`.

### `part_configure(len: i32)`

Where the board put this part. The buffer holds a `PartConfig`, `len` bytes:
which chip pin each of the part's own pins is wired to. Called once, before
`part_register`. Returns nothing.

A part models a *device*, not a placement — a button is a button wherever it
sits — so baking a pin number into a module would mean one module per
placement rather than one per device. Take the pins by your own names for them,
and treat a missing one as "not wired": a board may leave a pin unconnected, and
a part that guesses a number claims a line something else is using. A device
whose address pins choose its I2C address finds the board's choice in `addr`;
`0` means its own default.

A pin the board wired to the chip's EN (CHIP_PU) pin, its reset input, is not
in `pins`: EN has no GPIO number. Its name is in `en_pins`. A part that finds
one of its pins there claims EN (`EnClaim`) and drives it with `EnDrive`,
which holds the chip in reset while emulated time goes on; a part whose reset
output the board put on a GPIO instead drives that pin as any other.

A `PartConfig` that does not fit the buffer is not sent, and the part never
registers.

### `part_register() -> i32`

What this part answers for. Write a `Register` into the buffer and return its
length. Called once, right after `part_configure`, because a part cannot say
what it answers for until it knows where it is.

The `Register` carries `proto_version` (send `ProtoVersion.PROTO_VERSION_CURRENT`),
a `name` for logs, and `claims`: the pins this part drives, the pins it wants
every transition of (`GpioClaim.watch_pins`; any other pin reaches it as the
level once a tick), the I2C address it answers for on which pads, the
chip-select pad it is selected by on SPI, the serial line it is the device
on, the analog pads it puts a voltage on (`AnalogClaim`), the pins it reads
as PWM (`pwm_pins`) or as edges (`edge_pins`), the I2S data pads it is the
audio device on (`I2sClaim`), a camera sensor's bus (`DvpClaim`, `CsiClaim`),
a DSI panel's virtual channel (`DsiClaim`) and EN (`EnClaim`), all of which
follow from the configuration. A part nobody wired claims nothing. An analog
claim on a pad with no ADC channel is refused.

The host sends it to the emulator as this part's handshake. Returning `0` is an
error (`part_register returned nothing`). A refusal — a version mismatch, or a
pin another part already claims — is reported by the host, and the part is not
called again.

### `part_on_pins(levels: i64, now_ns: i64)`

The line level of every pin. Bit N is GPIO N, and a `1` bit means the line is
high, in the same encoding as `RegisterAck.pin_levels`: the level the chip
drives, released-high where it drives nothing. Bits above the chip's pin count
are `0`. `now_ns` is the emulated time of the change. Returns nothing.

Called once when the emulator accepts the handshake, with the levels at that
moment, then after every pin change, with the whole word each time, and once
after each chip reset, with the levels the reset left (see [When the chip
resets](#when-the-chip-resets)).

### `part_on_bus(len: i32) -> i32`

The guest is talking to an I2C address or an SPI chip select this part claims.
The buffer holds a `BusRequest`, `len` bytes. Write a `BusReply` into the buffer
and return its length. Optional: see [What a part exports](#what-a-part-exports).

#### On I2C

The request carries an `I2cTransaction`: the address, the bytes the master
wrote, and `read_len`, how many it reads back.

Always answer. Echo `correlation_id` and set `op` to `BUS_OP_I2C`. `ack = false`
is a NACK, the way to say "nothing is here". Otherwise `data` holds the
`read_len` bytes read, and nothing for a write-only transaction. Returning `0`
is an error, and so is a trap: the emulator stops while a transaction is out
(see below), and a part that does not answer stops the run, at once in the
native host and after 5 s of wall time in the browser, with the part named.

The emulator joins the pieces the driver splits a write into, so a register
read is one call with the register in `write` and the length in `read_len`. A
read the driver splits comes as several calls: each one after the first has
`continues` set and an empty `write`, and asks for the next bytes, which the
device sends from where its read left off. A read without `continues` is a new
one, even with an empty `write`. `I2cTransaction` in the `.proto` says which
reads are split. A call with an empty `write` and `read_len` of `0` is a
probe.

Only the part whose claim names the address is called. A request for an address
nobody claims never reaches a part: the emulator NACKs it.

#### On SPI

The request carries an `SpiTransfer`, and one request is one transfer: chip
select asserted, the phases clocked, chip select released, unless
`cs_keep_active` says the next request continues it. `tx` is the data phase the
master clocked out, and `rx_len` how many bytes it reads back. Answer a
transfer with an `rx_len` above `0` the way an I2C one is answered: echo
`correlation_id`, set `op` to `BUS_OP_SPI`, and put the `rx_len` bytes read in
`data`. A read through DMA is answered the same way; the emulator writes the
answer into the guest's buffer. Nothing acknowledges on SPI, so `ack = false`
is a MISO nobody drives: the master reads `0xFF`.

A transfer with `rx_len` `0` reads nothing. It completed when it was sent and
nothing waits for an answer, so return `0`: whatever is returned is not sent
on. A display or a DAC sees every write this way.

In full duplex, the driver's default, the bytes read come in while `tx` goes
out: byte N of the answer is what the device drives while byte N of `tx` is
clocked in. The driver clocks MOSI on every full-duplex read, so a read the
firmware gave no TX buffer still carries a `tx`: whatever the controller's
buffer held, which is no command to the device. After a DMA write on the
bus, a full-duplex read the firmware gave no TX buffer carries an empty
`tx`. A device added with
`SPI_DEVICE_HALFDUPLEX` reads after it writes, and a transfer that only reads
has an empty `tx`.

The rest of `SpiTransfer` is the framing. A device added with `command_bits` or
`address_bits` gets its instruction and address in `cmd` and `addr`, not in
`tx`; a part for a device driven with plain transfers finds its whole frame in
`tx` and `rx_len`. Then `dummy_cycles`, the line width of
each phase (`tx_lines`, `rx_lines`, and each phase's `lines`), `lsb_first`,
`three_wire` (a one-line reply comes in on D, not Q), `cs_keep_active` (the next
transfer continues this one, which is how an `esp_lcd` QSPI panel sends its
opcode word on one line and the pixels after it on four), the pad on each data
line (`line_pins`), and every pin's level as the transfer was clocked
(`pin_levels`): a display's D/C pin is read there, not from `part_on_pins`,
which only knows the level at the end of the emulator's pass.

Only the part on the transfer's chip select is called. A transfer on a chip
select nobody claims never reaches a part: the emulator answers it as an
undriven bus. A chip select the controller drives is found by its CS signal
on a pad. Firmware that drives a chip select itself as a plain GPIO
(`spics_io_num = -1`, as `esp_driver_sdspi` and IDF's `hd_eeprom` example do)
puts no CS signal on any pad: such a part names its clock or a data pad in
its claim, the emulator takes the bus from whichever of them the firmware put
a line on, and each transfer goes to the part whose chip-select pad is at its
selecting level as the transfer was clocked, low unless the claim's
`cs_active_high` says otherwise.

#### Either bus

Take as long as you need: no emulated time passes until the answer is back, so
the guest sees the same timing however long the call took. The emulator then
holds the answer until the bus would have finished clocking it, at the speed the
firmware programmed. Do not add that time yourself.

### `part_on_message(len: i32)`

The buffer holds an `EmulatorMessage` addressed to this part alone: a one-way
message, its `sim_time_ns` the instant it was sent and its payload saying what
it is about. Nothing comes back, and nothing waits: the guest is not stopped,
and a part with nothing to say says nothing. A part reads the payload's kind
and ignores a kind it does not take, as a host ignores an export it does not
know; a later channel adds kinds, not exports. Nine kinds today:

**`UartTx`**: what the chip transmitted on the serial line this part claimed
(`UartClaim`), in order, as it left the TX FIFO, from the moment the
firmware's driver put the UART on the claimed pads: what it sent before then
reached no device, as on a board. The bytes arrive in pieces of at most 128:
one frame may come in two calls and two frames in one, so a part that reads
frames keeps what it has until a frame is whole.

To answer, return a `UartRx` from `part_poll`: from the poll right after this
call to answer at once, or from a later one to answer after the device's own
delay. A `UartRx` is taken when it arrives, whatever its `sim_time_ns`; the
emulator does not hold it. Its bytes enter the chip's RX FIFO 128 at a time,
each next 128 no sooner than their time at the baud rate the firmware
programmed or 65,536 CPU cycles, whichever is less: a burst lands faster than
the wire would carry it, but never overruns the FIFO. Nothing in a `UartRx`
names the port: the emulator takes it from the pads the claim named. Bytes a
part sends before the firmware's driver has put a UART on those pads go
nowhere, as on a board.

**`PwmState`**: the period and the high time, in nanoseconds, of what a PWM
block (LEDC, or an MCPWM generator) now puts on a pin this part named in
`GpioClaim.pwm_pins`. Sent when it changes, never periodically: a steady
waveform is one call however long it runs, and the part keeps what it was
told. As a message it is at most 41 bytes, so any buffer holds it; one that
did not fit would fail the part, since it is not offered again.

`period_ns` of zero is no waveform: the block stopped or its timer is held,
the pin carries no PWM signal, or the output is not one pulse a period. The
pin's level is then in `part_on_pins`. A `high_ns` of zero is a pin held low,
and equal to `period_ns` one held high, both while the block's timer runs. A
servo reads `high_ns`, a buzzer `period_ns`, a lamp or a motor driver the two
together.

The first one comes right after the handshake if the pin carries a waveform
then, or once it does; a pin nothing ever drove is never reported. After a
chip reset every pin the part was told about is told again, period zero while
the firmware boots. Changes closer together than a quantum (1 ms by default)
come as one message per quantum with the waveform then, so a hardware fade is
seen in steps of at most a quantum and always ends on its last state. While a
PWM block toggles the pin, its level in `part_on_pins` stays at what it was
before the waveform started (or, for a part that attached meanwhile, at the
sample of it the handshake carried); it is reported once the waveform stops or
holds a level, whatever it is. A part that needs every edge names the pin in `watch_pins` as
well.

**`GpioEdges`**: a run of edges of a pin this part named in
`GpioClaim.edge_pins`, in `PulseDrive`'s shape: the pin reached
`first_level` at the message's `sim_time_ns` and toggled after each of
`widths_ns`. The level after the last width is open; the next run for the pin
starts with the edge that ends it, at its own time, so the gap between runs is
that level's width. The line is the pad as the chip drives it, whatever moves
it: an RMT, LEDC or MCPWM waveform, or the firmware's own writes. A run is
sent once it holds 512 widths, at each heartbeat and before a chip reset, so
it comes at most a heartbeat after its edges; right after it, `part_on_pins`
reports the pin at its last level, with the time of its last edge. A part
that must react inside that heartbeat names the pin in `watch_pins` instead.

A run is at most 2,581 bytes, so **a part that names `edge_pins` offers a
buffer of at least 2,581 bytes**; a host refuses a smaller one at load, and
one without `part_on_message`. A strip's frame of thousands of edges is a few
runs. A low the device ends a frame on, such as a WS2812's 50 µs latch, has no
edge after it: the part sees it end when time passes, in the next run's
start or a later call's time.

**`EnLevel`**: the chip's EN pin moved, at the message's `sim_time_ns`:
`level` false and the chip is held in reset from then, true and the last
holder let go, the chip reset following. A broadcast: every part that exports
`part_on_message` is given it, whether it claims EN or not, and whoever moved
the line, so a part that pulled EN sees its own pulse and one that shares the
line sees the others'. Four bytes at most.

**`I2sFormat`**: the layout of one direction of the I2S data pads this part
claimed (`I2sClaim`): `receive` is false for what the chip transmits and true
for what it expects to receive, then the frames per second, the slots in a
frame and the bits of one slot's container (8, 16, 24 or 32). Sent once the
firmware's driver has started that direction, the receive one only once its
rate was decoded from the clock the firmware programmed, and again whenever it
changes. Its `sim_time_ns` is when it took effect.

**`I2sOut`**: samples the chip clocked out on the data-out pad this part
claimed, laid out as the transmit format says: `channels` containers of `bits`
each, interleaved, little-endian, so a 24-bit sample is three bytes. At most
4096 bytes at a time, cut after a whole frame; its `sim_time_ns` is when the
last of them had been clocked out. Nothing waits on the part.

A part on a data-in pad supplies the chip's receiver with an `I2sIn` from
`part_poll`, in the receive format's layout. It is taken when it arrives,
whatever its `sim_time_ns`, and the receiver consumes it at the format's rate
in emulated time, so the part paces itself:

- Count from the receive `I2sFormat`'s `sim_time_ns`, and send nothing
  before it.
- Keep about 50 ms queued ahead of `now_ns`. The emulator holds about 96 KiB
  and drops the oldest past that; a receiver left short waits 20 ms or two of
  its buffers, whichever is longer, then takes silence.
- Send what is owed. Polls come at every heartbeat, which a CPU idle in WFI
  can stretch to a million CPU cycles (about 10 ms at 96 MHz), and
  `part_poll` is asked again until it returns 0 (up to 256 answers), so a
  buffer smaller than a heartbeat's worth of samples still keeps up.
- If `now_ns` has passed what it sent, go on from `now_ns`: samples for a
  time already past only add delay.
- If `now_ns` went backwards, the chip reset: send nothing until the next
  receive `I2sFormat`.

`examples/parts/inmp441` is a microphone that does this, and
`examples/parts/max98357a` an amplifier that listens.

**`DvpFormat`** and **`CsiFormat`**: what the camera receiver this part is the
sensor for looks like from the sensor's side. A part that claims a DVP sensor
(`DvpClaim`) is on the receiver's bus once the firmware routes the receiver's
pixel clock to the claimed pad, and is told then; one that claims the CSI host
(`CsiClaim`) is on the link from its claim. Sent when it changes, never per
frame, and with every field clear when the sensor leaves the bus. `capturing`
says whether a frame sent now is received; `session` counts the times the
receiver started capturing. A `DvpFormat` also carries the clock the chip
gives the sensor's XCLK pad (0 when none) and the polarities the firmware
expects; a `CsiFormat` the data lanes the host listens on. The picture
itself, its size and pixel format, is the sensor's: the firmware programs it
over SCCB, which reaches the part as I2C transactions to its address.

The sensor is the clock master, so nothing asks a part for its frame rate:
the rate is the gap between its frames' stamps. A part sends each frame as
`CamFrame` messages from `part_poll`, in pieces that each fit its own buffer:
the last with `end_of_frame`, the frame's width, height and fourcc (as
`esp_cosim.proto` spells them; a CSI sensor sends a Bayer mosaic) and the
`session` it last heard, stamped with the frame's start, its VSYNC. The other
pieces need no stamp. The receiver starts a frame at its stamp, or at once if
that has passed; it is taken when it arrives, not held by the emulator's event
queue. `part_poll` is called until it returns `0`, up to 256 answers, so a
frame of up to 256 pieces goes over at once; a larger one goes on over the
next calls, as the time a frame takes on the wire would.

- Send nothing before a format that says `capturing`, and count from its
  `sim_time_ns`.
- Send a frame no earlier than one frame period before its start. The DVP
  receiver keeps four and drops the oldest past that.
- If `now_ns` has passed the next start by more than a period (an idle chip
  waits in WFI, and emulated time leaps), start again from `now_ns`: frames
  stamped in the past would arrive back to back, faster than the sensor
  sends them.
- A format with a new `session`, or that says `capturing` is false: drop what
  is not sent. A frame for an ended session is lost anyway.
- On CSI, hold frames while `lanes` differs from the lanes the sensor
  transmits on: the host decodes nothing then.
- A raw frame is exactly its geometry long, or it is refused: the camera
  driver reports a frame's size from its buffer, whatever arrived.
- If `now_ns` went backwards, the chip reset: send nothing until the next
  format that says `capturing`.

A frame stamped `0` is paced by the receiver (one every 1/15 s on DVP, the CSI
host's `--csi-fps` on CSI), but a part then has no way to know when to send
the next one: stamp it.

`examples/parts/ov2640` is a DVP sensor that does this, and
`examples/parts/ov5647` a CSI one.

A part that claims a USB-OTG port as its host (`UsbHostClaim`) is the
computer the firmware's USB device is plugged into. A `UsbLink` tells it when
the device is there, after the bus reset, and at what speed; from then on it
sends `UsbTransfer` messages from `part_poll`, the first of them the
enumeration, and each is answered with a `UsbResult` handed over like any
other message, followed by a poll, so the next transfer can go out at once.
`esp_cosim.proto` says what a transfer is and how it ends.

- A result carries what the device sent, and a message that does not fit
  the buffer is not handed over: ask for no more (`UsbTransfer.length`) than
  the buffer holds beside some 40 bytes of envelope.
- Number the transfers (`id`): results of transfers on different endpoints
  come back in the order they end, not the order sent.
- A `UsbLink` that says not attached, or a result that says `NO_DEVICE`,
  voids what the part knew of the device: the firmware detached it or the
  chip reset. Enumerate again at the next `UsbLink` that says attached.

### `part_poll(now_ns: i64) -> i32`

How a part speaks without being asked. Write an `EmulatorMessage` into the
buffer and return its length, or return `0` for nothing to say. A button press
becomes a `GpioDrive`; a sensor's timed reply becomes one `PulseDrive` (see
[Playing a waveform](#playing-a-waveform)); a potentiometer turned becomes an `AnalogDrive`, the
millivolts on a pad it claimed, held until the next one and put back after a
chip reset without the part being asked; a signal sampled at a fixed rate
becomes `AnalogWave` pieces (see [Sending a sampled
voltage](#sending-a-sampled-voltage)). `now_ns` is the current emulated time.

Called after every pin change, after every `ClockSync` heartbeat (one per
quantum, 1 ms of emulated time by default), and right after `part_on_message` and
`part_on_input`. Each time, it is called again, with the same `now_ns`, until it
returns `0`, at most 256 times in a row, so a part hands over all it has to say
at once: a whole frame of drives stamped ahead, for one. Return `0` once
nothing is left; a part that keeps answering is cut off at the 256th answer,
and the rest waits for its next frame.

If the message is stamped with a `sim_time_ns` in the future, the emulator holds
it and applies it then; a `UartRx`, an `I2sIn` or a `CamFrame` is the exception, taken when it arrives. Stamp it with `now_ns`, or anything already past, and it
happens as soon as it can. See [Two ways to act at a
time](#two-ways-to-act-at-a-time-and-when-to-use-each).

### `part_on_input(len: i32)`

Somebody interacted with the part. The buffer holds the interaction's name as
UTF-8, `len` bytes: a plain name such as `"press"` or `"release"` that the part
defines. Returns nothing. Once the handshake is done, the host calls
`part_poll` right after, so the part can act at once.

A part ignores a name it does not have. That is what lets a part gain
interactions later without every UI being updated first, and what lets a UI
offer exactly the interactions a part understands without knowing what any of
them mean.

One form carries a number: `set:<name>=<value>`, for a quantity the part
measures, such as a sensor's temperature. `<name>` is lowercase letters, digits
and `-`. `<value>` is a plain decimal (an optional `-` and fraction, no
exponent) in the unit the part reports. The part clamps it to what its device
can read, so a UI or a script may pass on what a person typed. A part without
that quantity ignores it, like any other name. `examples/parts/sht31` takes
`set:temperature=` and `set:humidity=`.

### `part_state() -> i32`

What the part shows. Write a `PartState` into the buffer and return its
length, or return `0` for nothing to show. Optional: a part without it has
nothing to show, and a host says so to whoever asked.

Called whenever somebody looks: a page refreshing its view of the part, a
script reading a value (`part NAME state` on the command-line emulator's
control channel). Never by the emulator, and never at a time the part can
predict, so it is a snapshot of the state the part already keeps, not a place
to compute one. It may be called before the handshake is done.

`PartState.values` is named text in the part's own words: an LED says
`{"lit": "true"}`, a thermometer `{"temperature": "22.5"}`, a button
`{"pressed": "false"}`. The names are the part's, as its interactions are; a
UI shows what it is given. A quantity a part also takes through
`set:<name>=<value>` reports under the same name, so what was set reads back.

### `part_render() -> i32`

What the part draws: a lamp's brightness, a servo horn's angle, a display's
pixels. Write the bytes into the buffer and return their length, or return
`0` for nothing new to draw. Optional: a part without it draws nothing. The
command-line emulator says so to a script that asks; `part-worker.js`
answers as for nothing new.

Called whenever something draws the part: a page redrawing its picture of it,
a script reading the screen (`part NAME render` on the command-line
emulator's control channel). Never by the emulator, so a part nobody draws
costs nothing here. It may be called before the handshake is done.

The bytes are not a message, and neither host reads them: they reach whatever
draws the part as the part wrote them. Their layout is the part's to state
(the size, the pixel format, the order of the bytes), as its interactions'
names are; a part that draws a screen may also show its size and format in
`part_state`.

`0` means nothing is new since the part was last asked. Neither host keeps an
answer, so whatever draws the part keeps what it drew, and each call is
answered with what changed since the call before it, whoever made that one.
A part may answer only when its picture changed, so a still screen costs a
call and no copy, or answer every time, as a lamp's one byte may. The answer
is at most `part_buffer_len()` bytes, and a longer one is treated as `0`. A
picture larger than the buffer is the part's to cut up: it sends what
changed, a band at a time over several calls, and states how a band says
where it goes.

### `part_audio_out() -> i32`

The sound the part makes, for somebody to hear: what an amplifier puts on its
speaker, a buzzer, a synthesizer. Write samples into the buffer and return
their length in bytes, or return `0` for nothing new. Optional, and exported
only by a part that makes a sound: a host reads the export as "this part
makes a sound".

The samples are mono, 16-bit signed, little-endian, at the rate
`part_audio_rate(0)` gives. Never the chip's wire layout: how many slots an
I2S frame has, how wide they are and which of them the device plays are the
part's own business, and a host that had to know them would be modelling the
part.

Called whenever somebody listens: a page pulling it to play it. Never by the
emulator, so a part nobody listens to costs nothing here. Each call hands
over what the part made since the call before it, whoever made that one, at
most `part_buffer_len()` bytes; the part keeps the rest for the next call.
It keeps a bounded amount and drops the oldest: sound nobody collected is
delay, not data. An answer longer than the buffer, or of an odd number of
bytes, is treated as `0`.

### `part_audio_in(len: i32)`

A sound somebody makes at the part, for it to hear: a person speaking into a
microphone part. The buffer holds `len` bytes of mono, 16-bit signed,
little-endian samples at the rate `part_audio_rate(1)` gives. Returns
nothing. Optional, and exported only by a part that hears.

A host hands over a longer sound in pieces, in order, each at most
`part_buffer_len()` bytes of whole samples, so a part appends what it is
given. What it does with it is its own: a microphone puts it in its slot of
the frames it supplies the chip (`I2sIn`, from `part_poll`). Where the sound
came from, a person, a file or a test tone, is not said: the samples are the
same either way.

### `part_audio_rate(receive: i32) -> i32`

The samples a second the part makes its sound at (`receive` 0) or hears at
(`receive` 1), as `I2sFormat.receive` names the directions. `0` means it does
not know yet: a host then plays nothing and feeds nothing rather than guess a
rate, since a sound at the wrong rate plays at the wrong pitch with nothing to
say so. A part that makes or hears a sound exports it; without it a host reads
both rates as `0`.

The rate moves: the firmware programs the device's clock, and may program it
again, so a host asks again rather than keep the first answer. A part on I2S
reports the rate of the format it was last told; a part off I2S, such as a
synthesizer on a serial line, reports its own.

### `part_video_format() -> i32`

What the part asks to be shown, for whatever feeds it pictures: a webcam, a
folder of frames, a test card. Write a `VideoFormat` into the buffer (width,
height, fourcc and the pictures a second it sends) and return its length, or
return `0` for nothing wanted now. Optional, with `part_video_in`. A host
refuses a size whose RGB888 picture would be empty or more than 64 MiB, says
why, and shows the part nothing until it asks for another.

Called whenever something is about to show the part a picture, never by the
emulator. A camera sensor answers from its registers, which the firmware
programs over SCCB and reprograms, so whatever feeds it asks again before
each picture rather than once.

### `part_video_in(len: i32) -> i32`

A picture the part is shown: the buffer holds `len` bytes of one whole
picture, with no envelope, of the size the last `part_video_format` asked for:
a JPEG file when its fourcc is `JPEG`, else `width` × `height` pixels of RGB888
(red, green, blue, a byte each, rows top first). A feeder sends what it has (a
webcam's or a file's colours) and the part makes its own layout of them, so
every feeder sends the same bytes; a part may also take the fourcc's own
layout, which no feeder needs to make.

Return `0` when the part kept the picture, or write why it did not into the
buffer, as UTF-8, and return its length: a picture of the wrong size, one for
a mode the firmware has left since `part_video_format` answered, or a JPEG
that is not one. A picture larger than `part_buffer_len()` is not handed over.
Either way the host says so in the same words, `a N-byte picture does not
fit the part's buffer` or the part's own; a part that is shown pictures does
not ask for one larger than its buffer.

The emulator is not told. What the part does with the picture reaches the
chip as its own messages from `part_poll`: a camera sensor keeps the last
picture it was shown and sends it as its frames while it matches the mode its
registers select, so a JPEG-mode sensor sends the JPEG it was shown, and one
shown nothing sends what it would without a scene. `examples/parts/ov2640`
and `examples/parts/ov5647` do this, their colour bars standing in for a
scene. `part NAME video-format` and `part NAME video FILE` on the command-line
emulator's control channel (`ok`, or `err:` and why not), and the
`video_format` and `video_in` messages of `part-worker.js` (answered
`{ type: "video_in", ok, error }`), are the two hosts' ways to show a part a
picture.

### `memory`

The module's linear memory, exported as `memory`. The shared buffer lives in it.
The command-line host caps it at 64 MiB: a `memory.grow` past that fails,
returning −1 to the part as a grow does when the host has no room, and
nothing else happens. The browser host has no cap of its own (a worker's
memory is the browser's to limit), and neither host caps a table. A part
that needs more than that is modelling something other than a component on
a board.

### When a call fails

A trap in any call (an `unreachable`, an out-of-bounds access), a buffer offset
outside `memory`, or a `Register` or `BusReply` that does not decode is reported
by the host once, as this part's error, and the part is not called again. The
rest of the page and the emulator keep running, except that a bus transaction
the part was answering is never answered: the native host stops the run at
once, and the browser's pump after 5 s, naming the part. A message from
`part_poll` that does not decode is logged by the emulator and ignored.

## Two ways to act at a time, and when to use each

Say you model a motion sensor: it pulls its pin high, and must drop it two
seconds later on its own.

**Hold the deadline yourself.** You are polled with the current time on every
heartbeat, so keep the deadline in your own state and act when the clock passes
it. `part_on_input` is not told the time, so note the interaction there and
start the clock in the `part_poll` that follows it:

```rust
fn on_input(&mut self, name: &str) {
    if name == "detect" {
        self.detected = true;
    }
}

fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
    let pin = self.pin?;
    if std::mem::take(&mut self.detected) {
        self.release_at = Some(now_ns + 2_000_000_000); // two seconds from now
    }
    let high = self.release_at.is_some_and(|at| now_ns < at);
    if high == self.reported {
        return None; // speak only when the level changes
    }
    self.reported = high;
    let drive = GpioDrive { pin, level: high };
    Some(EmulatorMessage { sim_time_ns: now_ns, payload: Some(emulator_message::Payload::GpioDrive(drive)) })
}
```

`reported` starts `true`: a pin no part drives reads high, so the first poll
drives the sensor's resting level, low. A second `detect` while the pin is high
moves the deadline. `EmulatorMessage` and `GpioDrive` are the Rust types prost
generates from `esp_cosim.proto`.

Nothing is stored on your behalf, nothing can be lost, and you land within one
heartbeat — a millisecond. **This is the right answer for almost everything**: a
relay settling, a sensor conversion, a button auto-releasing.

**Or schedule ahead**, by stamping a future time. The emulator shortens its CPU
batch to land on it, so you get within a peripheral tick — about 1.6 µs —
instead of a millisecond. Use this only when a millisecond is genuinely too
coarse.

The queue that holds those events is bounded and **shared by every part**.
When it is full, a new event is refused, and `ClockSync.scheduled_dropped`
counts it — as a total across all parts, not yours. A part that holds its own
deadlines can never be refused.

## Playing a waveform

A device that speaks in timing on one wire, such as a DHT22's reply, a 1-Wire
presence pulse and its read slots, or an ultrasonic sensor's echo, returns
the whole waveform as one `PulseDrive`: the pin, the level it starts at, and
how long each level lasts in nanoseconds. The pin toggles after each width
and holds the level after the last one, so a reply that ends released ends
high:

```rust
// A DHT22's reply, 20-40 us after the host lets go: 80 us low, 80 us high,
// then each bit a 50 us low and a 26 us (0) or 70 us (1) high, then a last
// 50 us low before the line is released.
let mut widths = vec![80_000, 80_000];
for bit in bits {
    widths.extend([50_000, if bit { 70_000 } else { 26_000 }]);
}
widths.push(50_000);
let reply = PulseDrive { pin, first_level: false, widths_ns: widths };
Some(EmulatorMessage { sim_time_ns: released_ns + 30_000, payload: Some(emulator_message::Payload::PulseDrive(reply)) })
```

Each edge is scheduled as a `GpioDrive` stamped ahead is, so it lands within
a peripheral tick of its time, and whatever the firmware has on that pad sees
it: code reading the pin, a GPIO interrupt, a PCNT unit, an RMT receiver.

- **Late is kept whole.** A waveform stamped with a time that has passed
  starts at once and keeps every width, and its edges count in
  `ClockSync.events_fired_late`. A part answering inside a window the firmware
  opened cannot stamp later than the event it answers, and in the browser
  every answer comes a little after the emulator moved on: the reply then
  starts late, with its shape.
- **At most 512 widths.** A longer waveform goes as several messages, each
  stamped where the one before it ends: a waveform never starts before the
  end of the last one this part sent on the pin, so the pieces play back to
  back even when the first was late. More than 512 widths, or more edges than
  the shared queue has room for, and none of it is queued.
- **A zero width is a level held for no time**, and is skipped.
- **Size.** A `PulseDrive` of 512 widths is at most 2,581 bytes; a part sizes
  its buffer for the longest it writes. A DHT22's reply is under 300 bytes.

## Sending a sampled voltage

A part whose output is a signal rather than a setting, such as a microphone
envelope, a thermocouple following a heater, or a recorded trace, sends it
ahead of the chip as `AnalogWave` pieces: the pad, the period, and one
voltage a period. Voltage `k` holds from `sim_time_ns + k * period_ns`, the
last until the next voltage on the pad:

```rust
// 5 ms of a 50 Hz sine around 1.65 V, sampled at 10 kHz, from `from_ns`.
let period_ns = 100_000;
let millivolts = (0..50)
    .map(|k| {
        let t = (from_ns + k * period_ns) as f64 / 1e9;
        (1650.0 + 1000.0 * (2.0 * PI * 50.0 * t).sin()) as u32
    })
    .collect();
let piece = AnalogWave { pin, period_ns, millivolts };
Some(EmulatorMessage { sim_time_ns: from_ns, payload: Some(emulator_message::Payload::AnalogWave(piece)) })
```

Each voltage is held from its own nanosecond, wherever the chip's ticks fall,
and every conversion of the pad reads the one in force at its own instant: a
one-shot read, and each word of a continuous-mode frame.
[`examples/parts/ntc_thermistor`](../examples/parts/ntc_thermistor) sends
each change of temperature this way, as the thermistor warms or cools toward
it.

- **Send ahead.** Stamp each piece where the last one ends and keep a few
  milliseconds sent ahead of `now_ns`; the pieces then play as one signal.
- **A wave replaces what was sent for later.** It takes the place of every
  voltage the pad was set to from its first one's time on, so a part whose
  signal changes, a heater the firmware just turned on, sends the new wave
  from that moment. An `AnalogDrive` stamped inside a wave holds until the
  wave's next voltage.
- **Late keeps its times.** A wave stamped with a time that has passed skips
  the voltages whose time has passed and holds the newest of them from when
  it arrives; the rest keep their times. Unlike a `PulseDrive` it is not
  moved: a voltage is the signal's value at a time. The skipped ones count in
  `ClockSync.events_fired_late`.
- **At most 4096 voltages.** A pad keeps up to 4096 voltages ahead of the
  clock, and a wave that does not fit is refused whole. More than one voltage
  with a `period_ns` of 0 is refused.
- **Size.** An `AnalogWave` of 4096 voltages up to 3300 mV is at most 8,223
  bytes.

## When the chip resets

The firmware can reset the chip (`esp_restart`, a panic, a watchdog), and so
can whoever runs the emulator. The chip resets; **the board does not**. A real
sensor keeps its registers and a real button stays pressed when the MCU next
to it reboots, and a part is treated the same way:

- Its state is its own. Nothing is called to clear it, and a device that has
  no reset line of its own should not clear itself.
- What it drives stays driven. The emulator re-applies, to the rebuilt pins,
  the last level this part asked for on each pin it has driven, so a line held
  low before the reset reads low to the firmware after it. Do not re-send it;
  a part that speaks only when its level changes needs nothing extra.
- It is told. `part_on_pins` is called with the levels the reset left, as the
  chip drives them: every output the firmware had set is released, so most
  pins are back to high. No pin change is reported for that; the whole word
  is. `part_poll` follows, as after any frame. (On the wire this is a
  `ChipReset` on `SIM/RESET`, which the host turns into that call.)
- EN is the board's too. A part holding it low holds it across a reset from
  any other cause, and the chip stays in reset. A part is not reset by its
  own `EnDrive` either: the pulse is the chip's.
- Time starts again. `now_ns` in that call, and in every call after it, is
  smaller than before: the emulated clock restarted near zero. A deadline the
  part holds names a time on the old clock, so compare and treat it as passed
  or as void, whichever its device would do. A message the part stamped with a
  future time before the reset is dropped, not counted in
  `ClockSync.scheduled_dropped`, and never delivered. A waveform still playing
  is cut the same way, and its pin is held at the level the waveform ends on:
  a reply cut in a low does not leave the line low.
- Its PWM pins are told again: a `PwmState` with period zero while the
  firmware boots, then the waveform once its driver runs.
- Its I2S data pads go quiet until the firmware starts its I2S unit again:
  samples it supplied that the receiver had not taken are gone with the chip,
  and the receive `I2sFormat` comes again when the receiver is started.
- Its serial line goes quiet until the firmware's driver runs again. Bytes it
  had sent that the firmware had not read are gone with the chip's receiver,
  and bytes it sends before the driver puts a UART back on its pads go
  nowhere.
- Its camera receiver is told again: a format that says nothing captures (a
  DVP sensor is off the bus until the firmware routes the pixel clock again),
  then a new `session` once the firmware starts the receiver. Frames it had
  sent that had not started are gone with the chip.

**`DsiEvent`**: what the MIPI-DSI host sends the panel on the virtual
channel this part claimed (`DsiClaim`), one of three things. A `DsiCommand`
is a packet the firmware sent through the generic interface, as sent; a part
loaded after the firmware brought the panel up is handed the ones it missed
first. A `DpiFormat` is the video stream's size and pixel layout, sent when
it changes. A `DpiRows` is rows of a frame that differ from the last frame
this part was sent, in the frame buffer's own bytes: a still screen is no
call at all, and the first frame comes whole.

A frame's rows come in pieces of at most 65536 bytes of pixels, so **a part
that claims a DSI panel offers a buffer of at least 131072 bytes**; a host
refuses a smaller one at load. What the panel shows goes back out through
`part_render`, a band of the screen at a time when it is larger than the
buffer.

## What a part cannot claim yet

An SPI master with the chip as its slave (`SpiMasterClaim`): its
`SpiMasterTransfer` and `SpiMasterResult` have no route to a part, and both
hosts refuse a part that claims one. A host processor attaches over
`--cosim-tcp`.

The clock (`TimeClaim`): a part is called by its host and has no `TimeWaiting`
handed to it, so both hosts refuse a part that claims it. A simulator that
steps the chip attaches over `--cosim-tcp`.

## Versioning

Two numbers, both declared in `esp_cosim.proto`. They are checked separately
because they move for different reasons.

- **`part_abi_version()`** is the shape above, **version 1**. It changes when a
  *required* function is added or removed, or when one of them starts meaning
  something narrower. An *optional* export — one a host looks for before
  calling, rather than assuming: `part_on_bus`, `part_on_message`, `part_state`,
  `part_render`, `part_audio_out`, `part_audio_in`, `part_audio_rate`,
  `part_video_format`, `part_video_in` —
  moves nothing, so a module built before it existed is still called exactly
  the way it expects.
- **`Register.proto_version`** is the message contract. It is checked by the
  same handshake any consumer goes through, and refused the same way.

What this buys you: **a part built against an older emulator keeps working**, as
long as neither number moved. Adding a new optional field to a message, or a new
arm to a `oneof`, moves neither.

## Two hosts, one transcript

This ABI is implemented twice: by the command-line emulator, behind
`--cosim-board`, and by the browser's `part-worker.js`. Both are held to one
recorded transcript: the same parts, the same frames and interactions in, and
what each host sent on, failed on, showed, drew or did with sound after every step. The emulator's
tests replay it through both hosts, so a change to what a host does with a
part is a change to that transcript, reviewed as such.

## Time

A part never sleeps and never blocks. It is told `now_ns` and says what should
happen and when.

`now_ns` is the emulated clock. It is the same counter the guest firmware reads
through `esp_timer_get_time()`, so both sides already agree on the time. It has
nothing to do with real-world seconds.

One warning: a part that loops forever inside a call is lost. In the browser it
hangs the worker that runs it until the page terminates that worker, which the
demo page does not do; the native emulator stops a call that runs longer than
a second and treats the part as failed, as it does a trap. Avoiding it is the
part author's job.
