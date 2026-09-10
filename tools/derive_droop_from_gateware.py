#!/usr/bin/env python3
"""Derive the ANAN-G2 DDC droop-correction curve from the Saturn gateware's
own filter definition, instead of measuring it with a bench sweep.

WHY THIS EXISTS
---------------
The panadapter roll-off at the edges of a G2's DDC span is NOT CIC droop,
which is what our source comments have said since 2026-08-24. The Saturn
DDC chain is:

    ADC 122.88 MSPS -> CIC (6 stages, differential delay 1, R = 10..320)
                    -> FIR (1024 taps, decimate by 8, 18-bit coefficients)
                    -> DDC output at 122.88e6 / (R * 8)

which reproduces all six P2 rates exactly (R=320 -> 48 ksps ... R=10 ->
1536 ksps). Across the DDC OUTPUT band the CIC contributes at most 0.34 dB
and varies by 0.003 dB between the fastest and slowest rate. Essentially
all of the roll-off is the anti-alias FIR's transition band.

That FIR is fully specified in the Saturn repo, so the correction curve is
computable in closed form -- no radio required, no sweep, no per-unit
measurement, and identical at every rate by construction.

Sources (both GPL-3, same as AetherSDR):
  FPGA/sources/coefficientfiles/tx1024cfirImpulse.coe
      sha256 6ba2026d33ddbeb80684f6da71ff459742dd840c6cd7fbc036238ce1584ce027
      (name says "tx" and its comment calls it an example TX filter with CIC
      equaliser -- but it is what BOTH the standalone DDCIP project and the
      built saturn_project reference as the RX DDC FIR's CoefficientSource,
      and its response matches six independently measured rates to <1 dB.)
  FPGA/IP/DDCIP/.../DDC_Block_fir_compiler_0_0.xci   decimate 8, 2 channels
  FPGA/IP/DDCIP/.../DDC_Block_cic_compiler_{0,1}_0.xci  6 stages, dd 1

See spike/DECISIONS.md D-32.

USAGE
-----
  derive_droop_from_gateware.py --coe <path> [--emit-inc] [--verify <.inc>]
"""

import argparse
import hashlib
import math
import re
import sys

import numpy as np

# Must match AnanDroopCorrection.h's kDroopCorrectionFftSize.
NBINS = 1024
# FIR decimation, from DDC_Block_fir_compiler_0_0.xci "Decimation_Rate".
FIR_DECIM = 8
# CIC, from DDC_Block_cic_compiler_0_0.xci.
CIC_STAGES = 6
CIC_DIFF_DELAY = 1
# rate ksps -> CIC decimation R, i.e. 122.88e6 / (R * FIR_DECIM).
RATE_TO_R = {48: 320, 96: 160, 192: 80, 384: 40, 768: 20, 1536: 10}



def read_coe(path):
    raw = open(path, "rb").read()
    digest = hashlib.sha256(raw).hexdigest()
    text = raw.decode("utf-8", "replace")
    if "coefdata=" not in text:
        sys.exit(f"{path}: no 'coefdata=' section -- this does not look like a "
                 "Xilinx .coe coefficient file")
    body = text.split("coefdata=", 1)[1]
    taps = np.array([float(t) for t in
                     re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", body)])
    return taps, digest


def bin_frequencies():
    """f / fs_out for each FFT bin, DC at NBINS/2 -- the layout
    AnanSpectrum produces after its fftshift."""
    return (np.arange(NBINS) - NBINS // 2) / NBINS


def fir_response_db(taps):
    """FIR magnitude in dB relative to DC, sampled on our bin grid. The FIR
    runs at FIR_DECIM x the output rate, so bin frequency f/fs_out maps to
    f/fs_fir_in = (f/fs_out)/FIR_DECIM."""
    wn = bin_frequencies() / FIR_DECIM
    n = np.arange(len(taps))
    mag = np.abs(np.exp(-2j * np.pi * np.outer(wn, n)) @ taps)
    return 20.0 * np.log10(np.maximum(mag, 1e-300) / abs(taps.sum()))


def cic_response_db(R):
    """Standard CIC decimator response, in dB relative to DC."""
    x = np.pi * CIC_DIFF_DELAY * bin_frequencies() / (R * FIR_DECIM)
    safe = np.where(x == 0, 1.0, x)
    mag = np.where(x == 0, 1.0,
                   np.abs(np.sin(safe * R) / (R * np.sin(safe))))
    return CIC_STAGES * 20.0 * np.log10(mag)


# Matches AnanDroopCalibrator::computeCorrection()'s own default cap, so a
# shipped default and a bench sweep live in the same value domain. The raw
# curve runs to 209 dB at the outermost bin, which is meaningless -- there is
# no signal left there to recover -- and applyDroopCorrectionDb() adds without
# clamping, so an uncapped table would be a latent 209 dB spike the moment
# applyEdgeFade()'s tail shrank.
DEFAULT_CAP_DB = 90.0


def correction_table(taps, R, cap_db=DEFAULT_CAP_DB):
    """Per-bin dB to ADD to the measured spectrum -- the inverse of the
    chain's response, same sign convention as AnanDroopCorrection.h's
    applyDroopCorrectionDb()."""
    raw = -(fir_response_db(taps) + cic_response_db(R))
    return np.clip(raw, 0.0, cap_db)


def parse_measured_inc(path):
    """Pull the six measured tables out of a bench-captured .inc.

    NOTE this is the SIX-TABLE measured file, not the single-curve file this
    tool emits -- --verify compares the derivation against hardware, so it
    needs the measurement. Passing the shipped .inc finds nothing, and used to
    exit 0 having verified precisely nothing, which quietly turned the
    validation claim into an unbacked assertion.
    """
    text = open(path).read()
    tables = {int(m.group(1)):
              np.array([float(v) for v in re.findall(r"(-?\d+\.\d+)f", m.group(2))])
              for m in re.finditer(
                  r"kDroopDefault(\d+)Ksps\s*=\s*\{(.*?)\};", text, re.S)}
    if not tables:
        sys.exit(f"{path}: no kDroopDefault<N>Ksps tables found -- --verify "
                 "expects the six-table measured .inc, not the shipped "
                 "single-curve one")
    return tables


def emit_inc(table, digest, out):
    out.write("// GENERATED FILE -- do not edit by hand.\n")
    out.write("// Regenerate with derive_droop_from_gateware.py.\n//\n")
    out.write("// DERIVED, not measured: the Saturn DDC's own FIR coefficients\n")
    out.write("// (6-stage CIC + 1024-tap decimate-by-8 FIR), evaluated on our\n")
    out.write("// 1024-bin grid. One table covers all six DDC0 rates -- the FIR\n")
    out.write("// sees the same normalised frequency at every rate, and the CIC\n")
    out.write("// term varies by 0.003 dB across the whole range.\n//\n")
    out.write(f"// Source coefficients sha256 {digest}\n")
    out.write("// Saturn gateware 27. Clamped to [0, 90] dB, matching\n")
    out.write("// AnanDroopCalibrator::computeCorrection()'s own cap.\n\n")
    out.write("inline constexpr std::array<float, 1024> kDroopCorrectionGw27 = {\n")
    for i in range(0, NBINS, 8):
        row = ", ".join(f"{v:.4f}f" for v in table[i:i + 8])
        out.write(f"    {row},\n")
    out.write("};\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--coe", required=True,
                    help="path to the Saturn DDC FIR coefficient file "
                         "(tx1024cfirImpulse.coe in the Saturn FPGA repo)")
    ap.add_argument("--emit-inc", metavar="PATH",
                    help="write the C++ table to PATH ('-' for stdout)")
    ap.add_argument("--verify", metavar="INC",
                    help="compare against a measured AnanDroopDefaultTables.inc")
    ap.add_argument("--cap", type=float, default=DEFAULT_CAP_DB,
                    help="clamp the correction to [0, CAP] dB (default 90, "
                         "matching computeCorrection())")
    ap.add_argument("--crop", type=float, default=0.04,
                    help="SpectrumWidget kEdgeTaperFraction, for the "
                         "'what is actually on screen' report (default 0.04)")
    args = ap.parse_args()

    taps, digest = read_coe(args.coe)
    print(f"coefficients : {len(taps)} taps  sha256 {digest[:16]}...")

    table = correction_table(taps, R=40, cap_db=args.cap)  # any R; see spread check
    spread = max(np.max(np.abs(correction_table(taps, R, args.cap) - table))
                 for R in RATE_TO_R.values())
    print(f"rate spread  : {spread:.4f} dB across all six DDC0 rates "
          f"-- one table covers them all")

    k0 = int(NBINS * args.crop)
    shown = table[k0:NBINS - k0]
    print(f"crop {args.crop:<5}   : outermost displayed bin {k0} needs "
          f"{table[k0]:.2f} dB; {int((shown > 0.25).sum())} of {len(shown)} "
          f"displayed bins need >0.25 dB")
    if table[k0] < 0.25:
        print("               NOTE: at this crop the correction is a no-op "
              "-- the roll-off is already off-screen.")

    if args.verify:
        measured = parse_measured_inc(args.verify)
        # The band the correction actually works in: from the crop boundary
        # out to where the curve reaches zero (~8% in from each edge).
        lo, hi = int(NBINS * 0.04), int(NBINS * 0.08)
        idx = np.r_[lo:hi + 1, NBINS - hi - 1:NBINS - lo]
        mid = np.r_[NBINS // 8:NBINS - NBINS // 8]
        print("\nderived vs measured, roll-off region (bins "
              f"{lo}-{hi} and mirror):")
        for rate in sorted(measured):
            d = measured[rate][idx] - table[idx]
            print(f"  {rate:5d} ksps  mean {d.mean():+6.2f}  "
                  f"rms {np.sqrt((d ** 2).mean()):5.2f}  "
                  f"max|d| {np.abs(d).max():5.2f} dB")
        print("\nmid-band, where the derived curve is 0.000 dB "
              "(all of this is sweep noise -- see D-31):")
        for rate in sorted(measured):
            m = measured[rate][mid]
            print(f"  {rate:5d} ksps  mean {m.mean():+6.2f}  "
                  f"max {m.max():5.2f} dB  "
                  f"non-zero {int((m > 0).sum())}/{len(mid)} bins")

    if args.emit_inc:
        out = sys.stdout if args.emit_inc == "-" else open(args.emit_inc, "w")
        emit_inc(table, digest, out)
        if out is not sys.stdout:
            out.close()
            print(f"\nwrote {args.emit_inc}")


if __name__ == "__main__":
    main()
