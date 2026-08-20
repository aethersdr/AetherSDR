#!/usr/bin/env python3
"""Turn a bench calibration sweep into the ANAN-G2 droop-correction tables.

The Saturn FPGA's DDC0 decimation chain (two cascaded CIC decimators plus a
halfband FIR -- see reference/saturn/New_protocol_FPGA_Block_diagrams.pdf,
"Receiver(3)") imposes a real sin(x)/x amplitude droop near the edges of the
displayed span, baked into the raw IQ samples themselves. This script turns a
set of `radiocert spectrum` captures (one JSON file per capture, gathered via
the automation bridge across all 6 DDC0 rates with the antenna disconnected
or dummy-loaded, so the true receiver noise floor stands in as a flat
reference) into a per-bin dB correction table per rate, and writes the
generated C++ include consumed by src/core/backends/anan/AnanDroopCorrection.cpp.

Input: a directory of JSON files, each either a bare `measured` object
    {"ddc0RateKsps": ..., "fftSize": ..., "binsDbm": [...]}
or a full radiocert stage record {"id": "spectrum", "measured": {...}, ...} --
both shapes are accepted so a saved raw bridge_command response works as-is.

Usage:
    tools/anan_droop_calibration.py <capture_dir> [--cap-db 15]
        [--reference-window-fraction 0.15] [--min-captures-per-rate 5]

No app, no Qt, no build required -- pure static data processing.
"""

from __future__ import annotations

import argparse
import datetime
import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
OUTPUT = REPO / "src" / "core" / "backends" / "anan" / "AnanDroopCorrectionTables.inc"

# The six ANAN-G2 DDC0 rates (AnanBackend::capabilities()'s sampleRatesHz, in
# ksps rather than Hz) -- fixed by the radio, not derived from the captures.
VALID_RATES_KSPS = (48, 96, 192, 384, 768, 1536)

FFT_SIZE = 1024


def load_captures(capture_dir: Path) -> list[dict]:
    """Read every *.json file in capture_dir, unwrapping a radiocert stage
    record down to its `measured` object if present."""
    captures = []
    for path in sorted(capture_dir.glob("*.json")):
        with path.open("r", encoding="utf-8") as f:
            doc = json.load(f)
        measured = doc.get("measured", doc) if isinstance(doc, dict) else doc
        if not isinstance(measured, dict):
            continue
        if "ddc0RateKsps" not in measured or "binsDbm" not in measured:
            continue
        captures.append(measured)
    return captures


def group_by_rate(captures: list[dict]) -> dict[int, list[list[float]]]:
    grouped: dict[int, list[list[float]]] = {}
    for capture in captures:
        rate = int(capture["ddc0RateKsps"])
        bins = capture["binsDbm"]
        grouped.setdefault(rate, []).append(bins)
    return grouped


def median(values: list[float]) -> float:
    ordered = sorted(values)
    n = len(ordered)
    mid = n // 2
    if n % 2 == 1:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def median_power_curve(bin_arrays: list[list[float]]) -> list[float]:
    """Combine N per-capture dB curves of the same length into one, per bin,
    via the MEDIAN across captures -- robust against a stray in-band signal
    landing in one capture during a live-antenna sweep, unlike a mean.

    Converts to linear power before taking the median and back to dB after.
    For a median specifically this round trip is a no-op in exact arithmetic
    (dB is a strictly increasing function of power, and order statistics are
    invariant under any strictly monotonic transform -- whichever capture's
    value is the middle one stays the middle one either way). It is kept
    anyway so this function's contract matches "average in the physically
    meaningful domain" even if a future caller swaps the reducer for a mean,
    where the log-domain-bias problem is real (Jensen's inequality: the mean
    of the logs is not the log of the mean) and this conversion would start
    to matter."""
    if not bin_arrays:
        return []
    length = len(bin_arrays[0])
    curve = []
    for k in range(length):
        powers = [10.0 ** (bins[k] / 10.0) for bins in bin_arrays]
        curve.append(10.0 * math.log10(median(powers)))
    return curve


def reference_level(curve: list[float], window_fraction: float) -> float:
    """Median of the curve over a central window, not the exact center bin
    (avoids any residual DC-region artifact even though AnanSpectrum already
    DC-removes before windowing)."""
    n = len(curve)
    half_width = max(1, int(n * window_fraction / 2.0))
    center = n // 2
    window = curve[max(0, center - half_width):min(n, center + half_width)]
    return median(window)


def compute_correction(curve: list[float], reference_db: float,
                        cap_db: float) -> tuple[list[float], float]:
    """correction[k] = clamp(reference - measured[k], 0, cap_db). Returns the
    correction curve and the fraction of bins where the RAW (unclamped)
    correction exceeded cap_db -- i.e. genuinely unrecoverable, past the
    point where the CIC null approaches the noise floor and further gain
    would amplify noise rather than recover signal."""
    correction = []
    clamped = 0
    for value in curve:
        raw = reference_db - value
        if raw > cap_db:
            clamped += 1
        correction.append(max(0.0, min(cap_db, raw)))
    clamped_fraction = clamped / len(curve) if curve else 0.0
    return correction, clamped_fraction


def validate_curve(rate_ksps: int, correction: list[float], cap_db: float,
                    capture_count: int, min_captures: int) -> list[str]:
    problems = []
    if capture_count < min_captures:
        problems.append(
            f"rate {rate_ksps} ksps: only {capture_count} captures "
            f"(minimum {min_captures})")
    if len(correction) != FFT_SIZE:
        problems.append(
            f"rate {rate_ksps} ksps: {len(correction)} bins, expected {FFT_SIZE}")
    for k, value in enumerate(correction):
        if not math.isfinite(value):
            problems.append(f"rate {rate_ksps} ksps: bin {k} is not finite")
        elif value < 0.0 or value > cap_db:
            problems.append(
                f"rate {rate_ksps} ksps: bin {k} correction {value:.2f} dB "
                f"outside [0, {cap_db}]")
    return problems


def warn_bin_jumps(rate_ksps: int, correction: list[float],
                    jump_threshold_db: float) -> list[str]:
    """Non-fatal: real CIC ripple can be legitimately non-monotonic, but a
    single-bin spike this large next to smooth neighbours is more likely
    measurement noise than filter shape."""
    warnings = []
    for k in range(1, len(correction) - 1):
        left = abs(correction[k] - correction[k - 1])
        right = abs(correction[k] - correction[k + 1])
        if left > jump_threshold_db and right > jump_threshold_db:
            warnings.append(
                f"rate {rate_ksps} ksps: bin {k} looks like a single-bin "
                f"spike ({correction[k]:.2f} dB, neighbours "
                f"{correction[k - 1]:.2f}/{correction[k + 1]:.2f})")
    return warnings


def format_table(rate_ksps: int, correction: list[float]) -> str:
    name = f"kDroopTable{rate_ksps}Ksps"
    values = ", ".join(f"{v:.4f}f" for v in correction)
    return (
        f"constexpr DroopCorrectionTable {name}{{{{\n"
        f"    {values}\n"
        f"}}}};\n"
    )


def generate_inc_file(tables_by_rate: dict[int, list[float]],
                       capture_counts: dict[int, int], cap_db: float,
                       reference_window_fraction: float) -> str:
    generated_at = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%d %H:%M UTC")
    counts_line = ", ".join(
        f"{rate}ksps={capture_counts.get(rate, 0)}" for rate in VALID_RATES_KSPS)
    header = (
        "// AnanDroopCorrectionTables.inc\n"
        "//\n"
        "// DO NOT HAND-EDIT. Generated by tools/anan_droop_calibration.py from a\n"
        "// bench calibration sweep of the ANAN-G2's real DDC0 CIC/decimation droop\n"
        "// -- see AnanDroopCorrection.h for what this data is and why it exists.\n"
        "//\n"
        f"// Generated: {generated_at}\n"
        f"// Capture counts per rate: {counts_line}\n"
        f"// cap_db={cap_db}  reference_window_fraction={reference_window_fraction}\n"
        "//\n"
        "// Regenerate by running the calibration sweep against a real ANAN-G2 and\n"
        "// re-running this script; it overwrites this file.\n\n"
    )
    body = "\n".join(
        format_table(rate, tables_by_rate[rate])
        for rate in VALID_RATES_KSPS if rate in tables_by_rate)
    missing = [rate for rate in VALID_RATES_KSPS if rate not in tables_by_rate]
    for rate in missing:
        body += f"\nconstexpr DroopCorrectionTable kDroopTable{rate}Ksps{{}};" \
                 f"  // no captures for this rate -- no correction applied\n"
    return header + body + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture_dir", type=Path,
                        help="directory of radiocert-spectrum JSON captures")
    parser.add_argument("--cap-db", type=float, default=15.0,
                        help="maximum correction applied to any one bin (default 15)")
    parser.add_argument("--reference-window-fraction", type=float, default=0.15,
                        help="central window (as a fraction of bin count) used "
                             "as the flat reference level (default 0.15)")
    parser.add_argument("--min-captures-per-rate", type=int, default=5,
                        help="minimum captures required per rate (default 5)")
    parser.add_argument("--jump-threshold-db", type=float, default=6.0,
                        help="single-bin-spike warning threshold (default 6)")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the summary but do not write the .inc file")
    args = parser.parse_args()

    captures = load_captures(args.capture_dir)
    if not captures:
        print(f"no captures found in {args.capture_dir}", file=sys.stderr)
        return 1

    grouped = group_by_rate(captures)
    capture_counts = {rate: len(bins) for rate, bins in grouped.items()}

    tables_by_rate: dict[int, list[float]] = {}
    clamped_fractions: dict[int, float] = {}
    problems: list[str] = []
    warnings: list[str] = []

    for rate in sorted(grouped):
        curve = median_power_curve(grouped[rate])
        reference_db = reference_level(curve, args.reference_window_fraction)
        correction, clamped_fraction = compute_correction(
            curve, reference_db, args.cap_db)
        problems.extend(validate_curve(
            rate, correction, args.cap_db, capture_counts[rate],
            args.min_captures_per_rate))
        warnings.extend(warn_bin_jumps(rate, correction, args.jump_threshold_db))
        tables_by_rate[rate] = correction
        clamped_fractions[rate] = clamped_fraction
        print(f"rate {rate:5d} ksps: captures={capture_counts[rate]:3d} "
              f"reference={reference_db:7.2f} dB  "
              f"correction min/max={min(correction):.2f}/{max(correction):.2f} dB  "
              f"clamped_fraction={clamped_fraction:.4f}")

    for w in warnings:
        print(f"WARNING: {w}", file=sys.stderr)

    if problems:
        for p in problems:
            print(f"FAIL: {p}", file=sys.stderr)
        return 1

    if clamped_fractions:
        worst = max(clamped_fractions.values())
        suggested_taper = math.ceil((worst + 0.005) * 200.0) / 200.0  # round up to nearest 0.005
        print(f"\nSuggested SpectrumWidget.cpp kEdgeTaperFraction = {suggested_taper:.3f} "
              f"(worst-case clamped fraction {worst:.4f} + margin)")

    if args.dry_run:
        print("\n--dry-run: not writing", OUTPUT)
        return 0

    OUTPUT.write_text(
        generate_inc_file(tables_by_rate, capture_counts, args.cap_db,
                          args.reference_window_fraction),
        encoding="utf-8")
    print(f"\nwrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
