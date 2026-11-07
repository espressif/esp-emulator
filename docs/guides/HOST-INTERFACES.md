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
the pins it drives, the pins it wants every transition of, and the I2C
address or SPI chip select it answers. It then hears each watched pin at
every transition and every other pin once a tick, plus the heartbeat. It
drives pins at a named emulated time and answers bus transactions. When it
disconnects its claims are released, as for a device unplugged, and a level
it drove stays on the pin until something else drives it.

`--cosim-tcp` goes with `--cosim-board`: the parts and the connections share
one chip, and a pin one of them claims is refused to the other. The board's
parts make their claims before the first connection is taken, so a client
cannot take a pin from under a part.

Both directions carry length-prefixed frames, `[len: u32 LE][frame]`. From
the emulator, a frame is `[topic_len: u16 LE][topic][EmulatorMessage]`: a
broadcast carries its topic (`SIM/CLOCK`, `SIM/RESET`, `HW/GPIO/4/LEVEL`), a
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

`tools/cosim-lcd-view.py` is a device that answers the bus: an SPI display
(the ST7789 / ILI9341 command set, which `esp_lcd` and the raw
`spi_master/lcd` example both speak) that claims the panel's chip select,
reads D/C from the pin levels each transfer carries, answers the ID read, and
draws the pixels into a PNG, rewritten as the firmware fills the screen:

```sh
esp-emu --chip esp32s3 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5591 &
python3 tools/cosim-lcd-view.py 5591 --cs 10 --dc 9 --out lcd.png
```

A write completes as the firmware clocks it, so the panel never holds the
chip up; a read parks the firmware until the script answers. Together with
`--i2c-dev` for the board's expander and codecs, this is enough to watch a
product firmware's LVGL screen come up.

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
