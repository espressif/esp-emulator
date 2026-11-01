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
| `ping` | answers `ok` |

```sh
esp-emu --chip esp32c3 --firmware build/merged_flash.bin \
  --uart-tcp 127.0.0.1:5555 --control-tcp 127.0.0.1:5556

printf 'erase-region 0x9000 0x6000\nreset\n' | nc 127.0.0.1 5556   # wipe NVS, reboot
```

The emulator process keeps running across a reset, so a harness such as
pytest-embedded keeps one continuous output stream, expect history and log.
Flash operations act on the flash the running firmware sees; rewriting the image
file from outside would not, since the flash contents are carried across resets
in memory.

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
