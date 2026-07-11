#!/usr/bin/env python3
"""HL2 tune smoke test — set the RX1 NCO frequency (HPSDR Protocol 1, #4092/HL2).

Phase-0.3 spike (see README.md): prove we can point receiver 1 at a chosen
frequency via the C&C register map (grounded in hpsdr.py from the HL2 wiki, NOT
guessed), then confirm the radio keeps streaming and report the IQ level there.

Safety: only writes register 0x00 (config) and 0x02 (RX1 freq). Never touches
register 0x01 (TX NCO). Every C0 is even → MOX bit = 0 → cannot key the radio.

Run:  python3 tune.py --host 192.168.50.99 --freq 10000000 [--rate 48000] [--seconds 2]
      # --scan LOW HIGH STEP  → step RX1 across a range, print level per freq
"""

import argparse
import math
import socket
import sys
import time

import hpsdr


SPEED = {48000: 0, 96000: 1, 192000: 2, 384000: 3}


def dbfs(v: float) -> float:
    return 20 * math.log10(v / hpsdr.FULL_SCALE) if v > 0 else float("-inf")


def measure(sock, dst, freq: int, speed_code: int, seconds: float, seq0: int,
            gain_db: int = 20):
    """Tune RX1 to freq, stream for `seconds`, return (peak_dbfs, rms_dbfs, pkts, drops, seq).

    Round-robins the three registers a working RX needs: config (with the
    CONFIG_MERCURY ADC-select bit), ADC gain, and the RX1 NCO frequency. Sending
    only config+freq (no gain, and pre-fix no Mercury bit) yields flat noise."""
    seq = seq0
    regs = [hpsdr.cc_config(speed=speed_code, n_rx=1),
            hpsdr.cc_rx_gain(gain_db),
            hpsdr.cc_rx1_freq(freq)]
    ri = 0
    for _ in range(6):                       # prime: latch all three registers twice
        sock.sendto(hpsdr.ep2_packet(seq, regs[ri % 3], regs[(ri + 1) % 3]), dst)
        seq += 1; ri += 1

    pkts, samples, peak, sumsq, drops = 0, 0, 0, 0.0, 0
    exp = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        try:
            data, _ = sock.recvfrom(2048)
        except socket.timeout:
            continue
        r = hpsdr.parse_ep6(data)
        if r is None:
            continue
        s, n, pk, ss, _ = r
        if exp is not None and s != exp:
            gap = (s - exp) & 0xFFFFFFFF
            if gap < 0x80000000:             # forward gap = real loss; the reverse
                drops += gap                 # half = a reordered/dup packet
        exp = (s + 1) & 0xFFFFFFFF
        pkts += 1; samples += n; sumsq += ss
        if pk > peak:
            peak = pk
        sock.sendto(hpsdr.ep2_packet(seq, regs[ri % 3], regs[(ri + 1) % 3]), dst)
        seq += 1; ri += 1                    # keep all three registers refreshed, 1:1 pace
    rms = math.sqrt(sumsq / (2 * samples)) if samples else 0.0
    return dbfs(peak), dbfs(rms), pkts, drops, seq


def main() -> int:
    ap = argparse.ArgumentParser(description="HL2 RX1 tune smoke test")
    ap.add_argument("--host", default="192.168.50.99")
    ap.add_argument("--freq", type=int, default=10_000_000, help="RX1 Hz (default 10 MHz / WWV)")
    ap.add_argument("--rate", type=int, default=48000, choices=list(SPEED))
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--gain", type=int, default=20, help="LNA gain dB, -12..+48 (default 20)")
    ap.add_argument("--scan", nargs=3, type=int, metavar=("LOW", "HIGH", "STEP"),
                    help="sweep RX1 across LOW..HIGH by STEP (Hz), print level per freq")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    s.bind(("", 0)); s.settimeout(1.0)
    dst = (args.host, hpsdr.METIS_PORT)
    speed_code = SPEED[args.rate]

    s.sendto(hpsdr.metis_command(0x01), dst)   # start IQ
    seq = 0
    try:
        if args.scan:
            lo, hi, step = args.scan
            print(f"scan {lo/1e6:.3f}–{hi/1e6:.3f} MHz step {step/1e3:.0f} kHz "
                  f"@ {args.rate/1000:.0f} kHz, {args.seconds}s/step:")
            for f in range(lo, hi + 1, step):
                pk, rms, pkts, drops, seq = measure(s, dst, f, speed_code, args.seconds, seq, args.gain)
                bar = "#" * max(0, int((pk + 100) / 2))
                print(f"  {f/1e6:8.3f} MHz  peak {pk:+6.1f} dBFS  {bar}")
        else:
            print(f"→ tune RX1 = {args.freq/1e6:.6f} MHz @ {args.rate/1000:.0f} kHz "
                  f"(C0=0x{hpsdr.C0_RX1_FREQ:02x}, C1..C4={hpsdr.cc_rx1_freq(args.freq)[1:].hex()})")
            pk, rms, pkts, drops, seq = measure(s, dst, args.freq, speed_code, args.seconds, seq, args.gain)
            if pkts == 0:
                print("✗ no IQ after tune — check link / gateware."); return 1
            print(f"✓ streaming after tune: {pkts} pkts, {drops} dropped")
            print(f"  IQ level @ {args.freq/1e6:.3f} MHz: peak {pk:+.1f} dBFS, rms {rms:+.1f} dBFS")
            print("  (visual confirmation of the exact tune is spectrum.py's FFT)")
    finally:
        s.sendto(hpsdr.metis_command(0x00), dst); s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
