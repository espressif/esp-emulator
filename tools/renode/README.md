# An ESP chip in esp-emu as an SPI slave of a Renode machine

`EspEmuSpiBridge.cs` is an SPI device for [Renode](https://renode.io). The
Renode machine's SPI controller is the master; the device behind it is an ESP
chip running real firmware in esp-emu, with IDF's stock `spi_slave` driver.
It is for a board where an ESP is a co-processor of a host MCU and the host
side is emulated in Renode.

```
 Renode                                    esp-emu
 host MCU firmware                         ESP firmware (spi_slave driver)
   SPI controller ──► EspEmuSpiBridge ══► --cosim-tcp ──► GP-SPI in slave mode
   chip-select GPIO ──►      │      (TCP)
   handshake GPIO   ◄── Handshake ◄══════ GpioLevel of the handshake pad
```

Tested with Renode 1.17.0 (the portable Linux package) and its STM32F746
platform. No build step: Renode compiles the `.cs` file when a script
includes it.

## Use

Start esp-emu first, with the co-simulation socket:

```sh
esp-emu --chip esp32c6 --firmware build/merged_flash.bin --cosim-tcp 127.0.0.1:5590
```

Then, in the Renode script, before the platform is loaded:

```
include @/path/to/tools/renode/EspEmuSpiBridge.cs
machine LoadPlatformDescription @board.repl
```

and in the platform description, on top of the MCU's own:

```
using "platforms/cpus/stm32f746.repl"

esp: SPI.EspEmuSpiBridge @ spi1
    port: 5590          // esp-emu's --cosim-tcp port
    csPin: 14           // the ESP GPIO its spi_slave driver takes chip select on
    handshakePin: 2     // the ESP GPIO it raises when a transaction is loaded
    quantumUs: 100      // optional: hold the ESP's clock, see below

gpioPortA:
    4 -> syscfg#0@4 | esp@0       // the MCU's chip-select pin, low selects

esp:
    Handshake -> gpioPortB@0      // the handshake line into an MCU input
```

| Parameter | Default | Meaning |
|---|---|---|
| `host`, `port` | `127.0.0.1`, `5590` | where esp-emu listens |
| `csPin` | 14 | the ESP's chip-select GPIO (`spics_io_num`) |
| `handshakePin` | 2 | an ESP output the bridge mirrors onto its `Handshake` line |
| `sckHz` | 5000000 | the SPI clock the ESP's transaction timing is taken from |
| `quantumUs` | 0 | 0: each emulator runs at its own pace. Above 0: lockstep in quanta of this much time |

## How a transfer crosses

Renode's SPI controllers hand a device one byte at a time and want that
byte's answer at once. Each byte is sent to esp-emu as a piece of one
transfer with chip select held (`SpiMasterTransfer.hold_cs`), and the byte
the ESP's firmware loaded comes back in the `SpiMasterResult`. When the
chip-select line rises, an empty closing piece ends the transfer and the
ESP's transaction completes. The STM32 SPI model does not signal the end of
a transfer itself, so the chip-select GPIO must be wired to the bridge; a
controller that calls `FinishTransmission` needs no GPIO.

## Time

Without `quantumUs` the two emulators are not tied together: the handshake
line keeps a request/response protocol correct, but a timeout the host
firmware measures on Renode's clock is unrelated to the ESP's.

With `quantumUs` the Renode machine holds the ESP's clock (`TimeClaim`). The
ESP runs one quantum of emulated time for each quantum of the machine's
virtual time, and each waits for the other at the boundary, so neither is
ever more than a quantum ahead. A transfer, or a pin change, that arrives
while the ESP stands at the end of its quantum is taken there and answered
without waiting for the next grant.

The ESP has been running since esp-emu started, so its clock is ahead of the
machine's by however long Renode took to attach; from then on the two
advance by the same amount (`EspTimeNs - EspTimeAtAttachNs` against
`QuantaGranted`). While its clock is held esp-emu does not pace itself
against the host, so the pair runs as fast as the slower of the two.

## Check

`spi_slave_demo.py` runs the whole thing against IDF's
`examples/peripherals/spi_slave/receiver` and checks what each end read:

```sh
python3 tools/renode/spi_slave_demo.py \
    --renode ~/renode_1.17.0-portable/renode \
    --chip esp32c6 \
    --firmware receiver/build/merged_flash.bin --quantum-us 100
```

It plays the STM32's firmware through Renode's monitor, register by register,
so it needs no ARM toolchain. It runs the `esp-emu` on your `PATH`, or the one
`--emu` names; a release binary has `--cosim-tcp`, and a build from the
emulator's source needs `--features cosim`.

## Limits

- Full-duplex, one data line each way, whole bytes: what esp-emu's slave
  model takes. No half-duplex slave (`spi_slave_hd`).
- One exchange over the socket per byte.
- One handshake line. More lines, or a reset line into the ESP's EN, need
  more claims than this file makes.
- esp-emu must be up before Renode loads the platform, and must stay up:
  the bridge does not reconnect.
- Do not run Renode with `TMPDIR` pointing at a directory that holds other
  `renode-*` folders: Renode clears those as its own stale temporaries.
