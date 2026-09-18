#!/usr/bin/env python3
"""HL2 spectrum smoke test — FFT the raw IQ into a first panadapter (phase 0.4).

This is the payoff of the spike: it proves the whole raw-IQ data plane end to
end. Unlike Flex (which ships a hardware spectrum), HL2 ships raw IQ and the
*client* must FFT it. Here we tune RX1, capture a block of IQ, and render an
ASCII panadapter — the exact job the eventual Hl2Backend does internally before
emitting a normalized spectrumFrameReady.

Validated live: tuning to WWV (10 MHz) puts a strong carrier at baseband DC with
audible-band sidebands, and the noise floor sits ~50 dB down — a real, known
station resolved by our own FFT.

Protocol facts (register map, CONFIG_MERCURY ADC-select, LNA gain) are grounded
in hpsdr.py — consulted clean-room from the HL2 wiki + pihpsdr reference client
(see ../../THIRD_PARTY_LICENSES), never guessed. RX-only: every C0 is even so
MOX stays 0 — this cannot key the radio.

Run:  python3 spectrum.py --host 192.168.50.99 --freq 10000000 [--rate 48000]
                          [--gain 20] [--seconds 1.5] [--bins 64]
"""

import argparse
import socket
import sys
import time

import numpy as np

import hpsdr

SPEED = {48000: 0, 96000: 1, 192000: 2, 384000: 3}


def capture(sock, dst, freq, speed_code, gain_db, nsamp, settle):
    """Round-robin config+gain+freq, discard `settle` seconds, return complex IQ."""
    regs = [hpsdr.cc_config(speed=speed_code, n_rx=1),
            hpsdr.cc_rx_gain(gain_db),
            hpsdr.cc_rx1_freq(freq)]
    sock.sendto(hpsdr.metis_command(0x01), dst)          # start IQ
    seq = ri = 0
    I, Q = [], []
    drops = 0
    exp = None
    t0 = time.monotonic()
    try:
        while len(I) < nsamp and time.monotonic() - t0 < settle + 8:
            sock.sendto(hpsdr.ep2_packet(seq, regs[ri % 3], regs[(ri + 1) % 3]), dst)
            seq += 1; ri += 1
            try:
                data, _ = sock.recvfrom(2048)
            except socket.timeout:
                continue
            seq_rx = hpsdr.ep6_seq(data)     # cheap header read; iq_samples decodes
            if seq_rx is None:
                continue
            if exp is not None and seq_rx != exp:
                gap = (seq_rx - exp) & 0xFFFFFFFF
                if gap < 0x80000000:         # forward gap = real loss; the reverse
                    drops += gap             # half = a reordered/dup packet
            exp = (seq_rx + 1) & 0xFFFFFFFF
            if time.monotonic() - t0 > settle:           # let AGC/NCO settle first
                for i, q in hpsdr.iq_samples(data):
                    I.append(i); Q.append(q)
    finally:
        sock.sendto(hpsdr.metis_command(0x00), dst)      # stop
    iq = np.array(I[:nsamp], dtype=float) + 1j * np.array(Q[:nsamp], dtype=float)
    return iq, drops


def panadapter(iq, freq, rate, bins):
    """Render an ASCII spectrum, dBFS, DC-centered — the first HL2 panadapter."""
    if len(iq) < bins * 4:
        print(f"✗ too few samples ({len(iq)}) for a {bins}-bin FFT"); return
    dc = iq.mean()
    x = iq - dc                                          # drop the (large) DC offset
    w = np.hanning(len(x))
    X = np.fft.fftshift(np.fft.fft(x * w))
    mag = np.abs(X) / (np.sum(w) / 2)                    # coherent-gain normalized
    dbfs = 20 * np.log10(mag / hpsdr.FULL_SCALE + 1e-12)

    # bin the FFT down to `bins` columns across the full span, taking the peak per
    # column. array_split covers the whole spectrum (distributes the remainder), so
    # no high-frequency bins are dropped when len(dbfs) isn't a multiple of bins.
    cols = [chunk.max() for chunk in np.array_split(dbfs, bins)]
    lo, hi = min(cols), max(cols)
    dc_dbfs = 20 * np.log10(abs(dc) / hpsdr.FULL_SCALE + 1e-12)
    print(f"\n  RX1 {freq/1e6:.6f} MHz   span ±{rate/2000:.1f} kHz ({rate/1000:.0f} kHz)"
          f"   floor {lo:.0f} … peak {hi:.0f} dBFS   (DC offset {dc_dbfs:.0f} dBFS, removed)")
    print("  " + "─" * (bins + 12))
    for c in range(bins):
        off = (-rate / 2 + rate * c / bins) / 1000       # kHz from center
        v = cols[c]
        bar = int((v - lo) / max(1e-9, hi - lo) * 40)
        mark = "◆" if c == bins // 2 else " "            # center (tuned freq)
        print(f"  {off:+7.1f} kHz {v:6.1f} {mark}{'█' * bar}")


def main() -> int:
    ap = argparse.ArgumentParser(description="HL2 IQ → panadapter (phase 0.4)")
    ap.add_argument("--host", default="192.168.50.99")
    ap.add_argument("--freq", type=int, default=10_000_000, help="RX1 Hz (default 10 MHz / WWV)")
    ap.add_argument("--rate", type=int, default=48000, choices=list(SPEED))
    ap.add_argument("--gain", type=int, default=20, help="LNA gain dB, -12..+48")
    ap.add_argument("--seconds", type=float, default=1.5, help="capture length after settle")
    ap.add_argument("--bins", type=int, default=64, help="panadapter columns")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 21)
    s.bind(("", 0)); s.settimeout(1.0)
    dst = (args.host, hpsdr.METIS_PORT)

    nsamp = int(args.rate * args.seconds)
    print(f"→ capture {nsamp} IQ samples @ {args.rate/1000:.0f} kHz, "
          f"RX1={args.freq/1e6:.4f} MHz, gain {args.gain:+d} dB ...")
    iq, drops = capture(s, dst, args.freq, SPEED[args.rate], args.gain, nsamp, settle=0.6)
    s.close()
    if len(iq) < nsamp // 2:
        print(f"✗ only {len(iq)} samples — check link / gateware."); return 1
    if drops:
        print(f"  ({drops} packets dropped — WiFi jitter; prefer wired for clean IQ)")
    panadapter(iq, args.freq, args.rate, args.bins)
    print("\n  ◆ = tuned frequency (baseband DC). A carrier there = exact tune; "
          "a raised, shaped floor = live band noise. Data plane proven.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
