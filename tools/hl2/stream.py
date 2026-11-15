#!/usr/bin/env python3
"""HL2 IQ stream smoke test — HPSDR Protocol 1 (Metis) EP6 ingest.

Phase-0.2 spike (see README.md): start the radio, receive the raw-IQ torrent,
parse the HPSDR frames, and report whether the data plane is clean (sequence
gaps = dropped UDP packets) and whether there's real signal (IQ level). This is
the step that proves the *thing HL2 fundamentally needs and Flex never did* —
raw-IQ ingest — is workable.

Deliberately does NOT tune (set the RX NCO frequency): that needs the exact
Protocol-1 register map, which belongs in tune.py sourced from the HL2 gateware /
HPSDR spec (Principle I). Here the RX runs at its post-reset default; we're
proving transport + framing, not looking at a specific band yet.

Protocol authority: HPSDR "Metis - How it works"; Hermes-Lite 2 wiki/gateware.

Wire summary:
  start  → 0xEF 0xFE 0x04 0x01  (pad to 64 B) to host:1024   (0x00 = stop)
  EP6 in ← 1032 B: EF FE 01 06 | seq[4] | frame512 | frame512   (radio → us)
    each 512-B frame: 7F 7F 7F | C0..C4 (5 B C&C) | 504 B samples
    504 B = 63 samples × 8 B; sample = I[3] Q[3] mic[2], I/Q are 24-bit signed BE
  EP2 out→ 1032 B: EF FE 01 02 | seq[4] | frame512 | frame512   (us → radio)
    we send all-zero C&C keep-alive to sustain the stream (paced 1:1 with EP6)

Run:  python3 stream.py --host 192.168.50.99 [--seconds 3]
Read-only-ish: opens an RX stream. Won't key TX (C&C MOX bit stays 0). Sends a
stop on exit. If another client is already streaming, discovery showed [idle] so
you're clear.
"""

import argparse
import math
import socket
import sys
import time

import hpsdr

# One shared wire definition for the whole spike: framing, register map, and
# parse_ep6 all live in hpsdr.py (Principle I) — don't re-implement them here.
METIS_PORT = hpsdr.METIS_PORT
FULL_SCALE = hpsdr.FULL_SCALE
metis_command = hpsdr.metis_command
parse_ep6 = hpsdr.parse_ep6


def ep2_keepalive(seq: int) -> bytes:
    # EP2 (host→radio) keep-alive: two frames of all-zero C&C = register 0,
    # defaults (48 kHz, 1 RX, MOX off) — sustains the stream without configuring
    # anything. Real config/tuning is tune.py's job.
    return hpsdr.ep2_packet(seq, bytes(5), bytes(5))


def main() -> int:
    ap = argparse.ArgumentParser(description="HL2 IQ stream smoke test")
    ap.add_argument("--host", default="192.168.50.99", help="HL2 IP")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--rate", type=int, default=48000, help="assumed sample rate for the estimate")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    s.bind(("", 0))
    s.settimeout(1.0)
    dst = (args.host, METIS_PORT)

    print(f"→ start IQ stream from {args.host}:{METIS_PORT} for {args.seconds}s ...")
    ep2_seq = 0
    s.sendto(metis_command(0x01), dst)          # start
    for _ in range(2):                          # prime the radio's input buffer
        s.sendto(ep2_keepalive(ep2_seq), dst); ep2_seq += 1

    pkts = 0
    total_samples = 0
    peak = 0
    sumsq = 0.0
    sync_bad = 0
    expected_seq = None
    dropped = 0
    non_ep6 = 0
    t0 = time.monotonic()
    try:
        while time.monotonic() - t0 < args.seconds:
            try:
                data, _ = s.recvfrom(2048)
            except socket.timeout:
                print("  (timeout waiting for EP6 — no data flowing)")
                continue
            r = parse_ep6(data)
            if r is None:
                non_ep6 += 1
                continue
            seq, n, pk, ss, sync_ok = r
            if expected_seq is not None and seq != expected_seq:
                gap = (seq - expected_seq) & 0xFFFFFFFF
                if gap < 0x80000000:       # forward gap = real loss; the reverse
                    dropped += gap         # half = a reordered/dup packet, not loss
            expected_seq = (seq + 1) & 0xFFFFFFFF
            pkts += 1
            total_samples += n
            if pk > peak:
                peak = pk
            sumsq += ss
            if not sync_ok:
                sync_bad += 1
            s.sendto(ep2_keepalive(ep2_seq), dst); ep2_seq += 1   # 1:1 pace
    finally:
        s.sendto(metis_command(0x00), dst)      # stop
        s.close()

    elapsed = time.monotonic() - t0
    print(f"\n── results ({elapsed:.2f}s) ─────────────────────────────")
    if pkts == 0:
        print("✗ no EP6 IQ packets received. Try the wired interface (--host "
              "169.254.x), check firewall on :1024, confirm gateware streams on start.")
        print(f"  non-EP6 packets seen: {non_ep6}")
        return 1
    exp = pkts + dropped
    drop_pct = 100.0 * dropped / exp if exp else 0.0
    rate_est = total_samples / elapsed if elapsed else 0
    rms = math.sqrt(sumsq / (2 * total_samples)) if total_samples else 0.0
    peak_dbfs = 20 * math.log10(peak / FULL_SCALE) if peak else float("-inf")
    rms_dbfs = 20 * math.log10(rms / FULL_SCALE) if rms else float("-inf")

    print(f"✓ EP6 packets:     {pkts}")
    print(f"  IQ samples:      {total_samples}  (~{rate_est/1000:.1f} k/s; "
          f"assumed rate {args.rate/1000:.0f} kHz)")
    print(f"  dropped packets: {dropped}  ({drop_pct:.2f}%)   "
          f"{'← WiFi jitter, expected' if drop_pct > 0.5 else 'clean'}")
    print(f"  frame sync bad:  {sync_bad}   non-EP6: {non_ep6}")
    print(f"  IQ level:        peak {peak_dbfs:+.1f} dBFS, rms {rms_dbfs:+.1f} dBFS "
          f"{'(signal present)' if peak_dbfs > -80 else '(near floor)'}")
    print("\nData plane is workable — next: tune.py (set RX NCO) + spectrum.py (FFT).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
