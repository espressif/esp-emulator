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
(func (export "part_poll")      (param i64) (result i32))

(func (export "part_on_input")  (param i32))
(func (export "part_state")     (result i32))        ;; optional: what the part shows
(memory (export "memory") 1)
```

The first three are plumbing. The next five are the emulator's side. The last
two are the *person's* side — somebody clicking on the part, and somebody
looking at it. The emulator has no idea a person exists, so neither reaches it.

All but `part_on_bus` and `part_state` are required. A host checks for every
one when it loads a module, so a missing export is refused by name rather than
failing as a `TypeError` at its first call.

`part_on_bus` is needed only by a part that claims an I2C address or an SPI
chip select, and a part without it has no bus. A host refuses a part that claims
one but does not export it, at load, for the same reason: the first transaction
to it would otherwise wait for an answer that cannot come.

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
            part_poll                            after each pin change and heartbeat
reset       part_on_pins(levels after reset)     on each chip reset, then part_poll
person      part_on_input  ->  part_poll         on each interaction (part_poll
                                                 once the handshake is done)
            part_state                           whenever somebody looks
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
during one.

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

A `PartConfig` that does not fit the buffer is not sent, and the part never
registers.

### `part_register() -> i32`

What this part answers for. Write a `Register` into the buffer and return its
length. Called once, right after `part_configure`, because a part cannot say
what it answers for until it knows where it is.

The `Register` carries `proto_version` (send `ProtoVersion.PROTO_VERSION_CURRENT`),
a `name` for logs, and `claims`: the pins this part drives, the pins it wants
every transition of (`GpioClaim.watch_pins`; any other pin reaches it as the
level once a tick), the I2C address it answers for on which pads, and the
chip-select pad it is selected by on SPI, all of which follow from the
configuration. A part nobody wired claims nothing.

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

### `part_poll(now_ns: i64) -> i32`

How a part speaks without being asked. Write an `EmulatorMessage` into the
buffer and return its length, or return `0` for nothing to say. A button press
becomes a `GpioDrive`. `now_ns` is the current emulated time.

Called after every pin change, after every `ClockSync` heartbeat (one per
quantum, 1 ms of emulated time by default), and right after `part_on_input`.

If the message is stamped with a `sim_time_ns` in the future, the emulator holds
it and applies it then. Stamp it with `now_ns`, or anything already past, and it
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
- Time starts again. `now_ns` in that call, and in every call after it, is
  smaller than before: the emulated clock restarted near zero. A deadline the
  part holds names a time on the old clock, so compare and treat it as passed
  or as void, whichever its device would do. A message the part stamped with a
  future time before the reset is dropped, not counted in
  `ClockSync.scheduled_dropped`, and never delivered.

## Versioning

Two numbers, both declared in `esp_cosim.proto`. They are checked separately
because they move for different reasons.

- **`part_abi_version()`** is the shape above, **version 1**. It changes when a
  *required* function is added or removed, or when one of them starts meaning
  something narrower. An *optional* export — one a host looks for before
  calling, rather than assuming: `part_on_bus`, `part_state` — moves nothing,
  so a module built before it existed is still called exactly the way it
  expects.
- **`Register.proto_version`** is the message contract. It is checked by the
  same handshake any consumer goes through, and refused the same way.

What this buys you: **a part built against an older emulator keeps working**, as
long as neither number moved. Adding a new optional field to a message, or a new
arm to a `oneof`, moves neither.

## Two hosts, one transcript

This ABI is implemented twice: by the command-line emulator, behind
`--cosim-board`, and by the browser's `part-worker.js`. Both are held to one
recorded transcript: the same parts, the same frames and interactions in, and
what each host sent on, failed on or showed after every step. The emulator's
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
