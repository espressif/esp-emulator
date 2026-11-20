//
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0
//
// An SPI device for Renode that is really an ESP chip running in esp-emu.
//
// The Renode machine's SPI controller is the master. Each byte it clocks goes
// to esp-emu's co-simulation socket (--cosim-tcp) as a piece of one transfer
// into the chip's GP-SPI in slave mode, chip select held, and the byte the
// ESP's firmware answers comes straight back. The chip-select line and one
// handshake line are GPIOs. See README.md beside this file.
//
// Renode compiles this file itself: `include @EspEmuSpiBridge.cs` in a
// script, then in a platform description
//
//     esp: SPI.EspEmuSpiBridge @ spi1
//         port: 5590
//         csPin: 14
//         handshakePin: 2
//         quantumUs: 100
//
//     gpioPortA:
//         4 -> esp@0                  // the master's chip select, low selects
//
//     esp:
//         Handshake -> gpioPortB@0    // the ESP's "transaction loaded" line
//
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using Antmicro.Renode.Core;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals.Timers;

namespace Antmicro.Renode.Peripherals.SPI
{
    public class EspEmuSpiBridge : ISPIPeripheral, IGPIOReceiver
    {
        // host, port:    where esp-emu listens (--cosim-tcp HOST:PORT)
        // csPin:         the ESP GPIO its spi_slave driver takes chip select on
        // handshakePin:  the ESP GPIO it raises when a transaction is loaded
        // sckHz:         the clock the master is taken to drive, for the ESP's timing
        // quantumUs:     0 lets the two emulators run each at its own pace. Above 0
        //                this machine holds the ESP's clock: the ESP runs one
        //                quantum of emulated time for each quantum of this
        //                machine's virtual time, and each waits for the other at
        //                the boundary.
        public EspEmuSpiBridge(IMachine machine, string host = "127.0.0.1", int port = 5590, int csPin = 14,
            int handshakePin = 2, int sckHz = 5000000, ulong quantumUs = 0)
        {
            this.handshakePin = handshakePin;
            this.sckHz = sckHz;
            this.quantumNs = quantumUs * 1000;
            Handshake = new GPIO();

            // The stream owns the socket from here on.
            var client = new TcpClient(host, port) { NoDelay = true };
            stream = client.GetStream();

            // Register: the SPI master on the chip-select pad, watching the
            // handshake pad, and holding the clock if asked to.
            var claims = new MemoryStream();
            Ld(claims, Proto.RegisterClaims, Msg(m => Ld(m, Proto.ClaimSpiMaster, Msg(c => Varint(c, Proto.SpiMasterClaimCsPin, (ulong)csPin)))));
            Ld(claims, Proto.RegisterClaims, Msg(m => Ld(m, Proto.ClaimGpio, Msg(c => Varint(c, Proto.GpioClaimWatchPins, 1UL << handshakePin)))));
            if(quantumNs > 0)
            {
                Ld(claims, Proto.RegisterClaims, Msg(m => Ld(m, Proto.ClaimTime, new byte[0])));
            }
            Send(Msg(m => Ld(m, Proto.Register, Msg(r =>
            {
                Varint(r, Proto.RegisterProtoVersion, Proto.Version);
                Ld(r, Proto.RegisterName, Encoding.ASCII.GetBytes("renode"));
                r.Write(claims.ToArray(), 0, (int)claims.Length);
            }))));

            new Thread(ReadLoop) { IsBackground = true, Name = "esp-emu cosim reader" }.Start();

            if(quantumNs > 0)
            {
                // The ESP stops where this registered and says so. Only then
                // is the first quantum granted, so that every later notice
                // is the end of a quantum: the ESP runs each one while this
                // machine runs its own, and each boundary waits for it.
                if(!espWaiting.Wait(ReplyTimeoutMs))
                {
                    this.Log(LogLevel.Error, "esp-emu did not stop for this machine's clock within {0} ms", ReplyTimeoutMs);
                }
                EspTimeAtAttachNs = Interlocked.Read(ref espTimeNs);
                Grant();
                quantum = new LimitTimer(machine.ClockSource, 1000000, this, "quantum", limit: quantumUs,
                    enabled: true, eventEnabled: true);
                quantum.LimitReached += OnQuantum;
            }
        }

        // The ESP raises this when a transaction is loaded.
        public GPIO Handshake { get; }

        // Bytes exchanged, and quanta granted, for a script to read.
        public long BytesExchanged { get; private set; }

        public long QuantaGranted { get; private set; }

        // The ESP's emulated time as last heard, and when this attached (with
        // a quantum: where it stopped for this machine's clock), in
        // nanoseconds. With a quantum, the first less the second is what was
        // granted, within one quantum.
        public long EspTimeNs => Interlocked.Read(ref espTimeNs);

        public long EspTimeAtAttachNs { get; private set; }

        // The chip-select line: low selects.
        public void OnGPIO(int number, bool value)
        {
            if(value == !selected)
            {
                return;
            }
            selected = !value;
            if(!selected && held)
            {
                // Chip select rises: a closing piece with no bytes, and the
                // ESP's transaction completes.
                Exchange(new byte[0], false);
                held = false;
            }
        }

        public byte Transmit(byte data)
        {
            held = true;
            BytesExchanged++;
            var reply = Exchange(new[] { data }, true);
            // No slave selected: nothing drives MISO.
            return reply != null && reply.Length > 0 ? reply[0] : (byte)0xFF;
        }

        public void FinishTransmission()
        {
            // Controllers that signal the end of a transfer this way rather
            // than through a chip-select GPIO.
            OnGPIO(0, true);
        }

        public void Reset()
        {
            selected = false;
            held = false;
        }

        // One SpiMasterTransfer, and the bytes its SpiMasterResult brings; null
        // when the ESP has no slave on the chip select.
        private byte[] Exchange(byte[] tx, bool holdCs)
        {
            var id = (ulong)Interlocked.Increment(ref nextId);
            Send(Msg(m =>
            {
                Varint(m, Proto.SimTimeNs, (ulong)Interlocked.Read(ref espTimeNs));
                Ld(m, Proto.SpiMasterTransfer, Msg(t =>
                {
                    Varint(t, Proto.TransferId, id);
                    Ld(t, Proto.TransferTx, tx);
                    Varint(t, Proto.TransferSckHz, (ulong)sckHz);
                    if(holdCs)
                    {
                        Varint(t, Proto.TransferHoldCs, 1);
                    }
                }));
            }));
            while(true)
            {
                if(!results.TryTake(out var r, ReplyTimeoutMs))
                {
                    this.Log(LogLevel.Error, "esp-emu did not answer SPI transfer {0} within {1} ms", id, ReplyTimeoutMs);
                    return null;
                }
                if(r.Id == id)
                {
                    return r.Selected ? r.Rx : null;
                }
            }
        }

        // The end of a quantum of this machine's virtual time: wait for the
        // ESP to have used up its own, then let it run the next.
        private void OnQuantum()
        {
            if(!espWaiting.Wait(ReplyTimeoutMs))
            {
                this.Log(LogLevel.Warning, "esp-emu has not reached the end of its quantum after {0} ms", ReplyTimeoutMs);
            }
            Grant();
        }

        private void Grant()
        {
            espWaiting.Reset();
            Send(Msg(m => Ld(m, Proto.TimeGrant, Msg(g => Varint(g, Proto.TimeGrantAdvanceNs, quantumNs)))));
            QuantaGranted++;
        }

        private void ReadLoop()
        {
            try
            {
                var head = new byte[4];
                while(true)
                {
                    ReadExact(head, 4);
                    var frame = new byte[BitConverter.ToInt32(head, 0)];
                    ReadExact(frame, frame.Length);
                    var topicLen = BitConverter.ToUInt16(frame, 0);
                    var msg = Decode(frame, 2 + topicLen, frame.Length);
                    if(msg.TryGetValue(Proto.SimTimeNs, out var t))
                    {
                        Interlocked.Exchange(ref espTimeNs, (long)(ulong)t);
                    }
                    if(msg.TryGetValue(Proto.RegisterAck, out var ackRaw))
                    {
                        var ack = Decode((byte[])ackRaw);
                        if(!ack.ContainsKey(Proto.AckAccepted))
                        {
                            this.Log(LogLevel.Error, "esp-emu refused the registration: {0}",
                                ack.TryGetValue(Proto.AckRejectReason, out var why) ? Encoding.UTF8.GetString((byte[])why) : "?");
                        }
                        else
                        {
                            // The firmware may have raised the pad before this attached.
                            var levels = ack.TryGetValue(Proto.AckPinLevels, out var lv) ? (ulong)lv : 0;
                            Handshake.Set((levels >> handshakePin & 1) != 0);
                            if(quantumNs == 0)
                            {
                                EspTimeAtAttachNs = Interlocked.Read(ref espTimeNs);
                            }
                            this.Log(LogLevel.Info, "attached to esp-emu");
                        }
                    }
                    if(msg.TryGetValue(Proto.GpioLevel, out var gpioRaw))
                    {
                        var g = Decode((byte[])gpioRaw);
                        var pin = g.TryGetValue(Proto.GpioLevelPin, out var p) ? (int)(ulong)p : 0;
                        if(pin == handshakePin)
                        {
                            Handshake.Set(g.TryGetValue(Proto.GpioLevelLevel, out var l) && (ulong)l != 0);
                        }
                    }
                    if(msg.TryGetValue(Proto.SpiMasterResult, out var resRaw))
                    {
                        var r = Decode((byte[])resRaw);
                        results.Add(new Result
                        {
                            Id = r.TryGetValue(Proto.ResultId, out var i) ? (ulong)i : 0,
                            Selected = r.TryGetValue(Proto.ResultSelected, out var s) && (ulong)s != 0,
                            Rx = r.TryGetValue(Proto.ResultRx, out var rx) ? (byte[])rx : new byte[0],
                        });
                    }
                    if(msg.ContainsKey(Proto.TimeWaiting))
                    {
                        espWaiting.Set();
                    }
                }
            }
            catch(Exception e)
            {
                this.Log(LogLevel.Warning, "esp-emu connection closed: {0}", e.Message);
                // Nothing will answer any more: do not hold this machine's clock.
                espWaiting.Set();
            }
        }

        private void ReadExact(byte[] buf, int count)
        {
            var at = 0;
            while(at < count)
            {
                var n = stream.Read(buf, at, count - at);
                if(n <= 0)
                {
                    throw new EndOfStreamException("esp-emu left");
                }
                at += n;
            }
        }

        private void Send(byte[] msg)
        {
            try
            {
                // One write: the socket does not hold a short one back, and a
                // frame in two would be two segments for every byte clocked.
                var frame = new byte[4 + msg.Length];
                BitConverter.GetBytes(msg.Length).CopyTo(frame, 0);
                msg.CopyTo(frame, 4);
                lock(stream)
                {
                    stream.Write(frame, 0, frame.Length);
                }
            }
            catch(IOException)
            {
                // esp-emu left; the reader says so once.
            }
        }

        // ---- protobuf by hand: the few messages of proto/esp_cosim.proto used
        // here ----

        // That file's field numbers, by the names it gives them. They never
        // change once shipped; a message this file does not know is skipped.
        private static class Proto
        {
            public const ulong Version = 1;             // ProtoVersion.PROTO_VERSION_CURRENT

            // EmulatorMessage
            public const int SimTimeNs = 1;
            public const int GpioLevel = 3;
            public const int Register = 4;
            public const int RegisterAck = 5;
            public const int SpiMasterTransfer = 38;
            public const int SpiMasterResult = 39;
            public const int TimeGrant = 40;
            public const int TimeWaiting = 41;

            // Register, and the Claim kinds it lists
            public const int RegisterProtoVersion = 1;
            public const int RegisterName = 2;
            public const int RegisterClaims = 3;
            public const int ClaimGpio = 1;
            public const int ClaimSpiMaster = 22;
            public const int ClaimTime = 23;
            public const int GpioClaimWatchPins = 2;
            public const int SpiMasterClaimCsPin = 1;

            // RegisterAck
            public const int AckAccepted = 5;
            public const int AckRejectReason = 6;
            public const int AckPinLevels = 7;

            // GpioLevel
            public const int GpioLevelPin = 1;
            public const int GpioLevelLevel = 2;

            // SpiMasterTransfer and SpiMasterResult
            public const int TransferId = 1;
            public const int TransferTx = 2;
            public const int TransferSckHz = 3;
            public const int TransferHoldCs = 4;
            public const int ResultId = 1;
            public const int ResultSelected = 2;
            public const int ResultRx = 3;

            // TimeGrant
            public const int TimeGrantAdvanceNs = 1;
        }

        private static byte[] Msg(Action<MemoryStream> fill)
        {
            var m = new MemoryStream();
            fill(m);
            return m.ToArray();
        }

        private static void RawVarint(MemoryStream m, ulong v)
        {
            while(v > 0x7F)
            {
                m.WriteByte((byte)((v & 0x7F) | 0x80));
                v >>= 7;
            }
            m.WriteByte((byte)v);
        }

        private static void Varint(MemoryStream m, int field, ulong v)
        {
            RawVarint(m, (ulong)field << 3);
            RawVarint(m, v);
        }

        private static void Ld(MemoryStream m, int field, byte[] inner)
        {
            RawVarint(m, ((ulong)field << 3) | 2);
            RawVarint(m, (ulong)inner.Length);
            m.Write(inner, 0, inner.Length);
        }

        private static Dictionary<int, object> Decode(byte[] buf)
        {
            return Decode(buf, 0, buf.Length);
        }

        private static Dictionary<int, object> Decode(byte[] buf, int at, int end)
        {
            var fields = new Dictionary<int, object>();
            while(at < end)
            {
                var tag = ReadVarint(buf, ref at);
                var field = (int)(tag >> 3);
                switch((int)(tag & 7))
                {
                case 0:
                    fields[field] = ReadVarint(buf, ref at);
                    break;
                case 1:
                    at += 8;
                    break;
                case 2:
                {
                    var n = (int)ReadVarint(buf, ref at);
                    var inner = new byte[n];
                    Array.Copy(buf, at, inner, 0, n);
                    fields[field] = inner;
                    at += n;
                    break;
                }
                case 5:
                    at += 4;
                    break;
                default:
                    return fields;
                }
            }
            return fields;
        }

        private static ulong ReadVarint(byte[] buf, ref int at)
        {
            ulong v = 0;
            var shift = 0;
            while(true)
            {
                var b = buf[at++];
                v |= (ulong)(b & 0x7F) << shift;
                shift += 7;
                if((b & 0x80) == 0)
                {
                    return v;
                }
            }
        }

        private struct Result
        {
            public ulong Id;
            public bool Selected;
            public byte[] Rx;
        }

        // How long this machine waits on esp-emu, on the host's clock, before
        // it says so and goes on.
        private const int ReplyTimeoutMs = 5000;

        private readonly int handshakePin;
        private readonly int sckHz;
        private readonly ulong quantumNs;
        private readonly NetworkStream stream;
        // Kept so the timer lives as long as this does.
        private readonly LimitTimer quantum;
        private readonly BlockingCollection<Result> results = new BlockingCollection<Result>();
        private readonly ManualResetEventSlim espWaiting = new ManualResetEventSlim(false);
        private bool selected;
        private bool held;
        private long nextId;
        private long espTimeNs;
    }
}
