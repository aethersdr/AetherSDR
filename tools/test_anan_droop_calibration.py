#!/usr/bin/env python3
"""Unit tests for tools/anan_droop_calibration.py, on synthetic data only --
no bench sweep needed."""

import json
import math
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anan_droop_calibration as cal  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def test_median_odd():
    check(cal.median([3.0, 1.0, 2.0]) == 2.0, "median of 3 values")


def test_median_even():
    check(cal.median([1.0, 3.0]) == 2.0, "median of 2 values")


def test_median_power_curve_rejects_a_stray_outlier_capture():
    # Bin 0: all five captures agree (-10 dB). Bin 1: four quiet captures
    # (-10 dB) and one huge outlier (+40 dB, e.g. a stray signal during a
    # live-antenna sweep) -- the median must ignore the outlier entirely,
    # which a mean could not.
    quiet = [-10.0, -10.0]
    outlier = [-10.0, 40.0]
    curve = cal.median_power_curve([quiet, quiet, quiet, quiet, outlier])
    check(abs(curve[0] - (-10.0)) < 1e-6, f"bin 0 should stay -10 dB, got {curve[0]}")
    check(abs(curve[1] - (-10.0)) < 1e-6,
          f"bin 1 median should reject the outlier, got {curve[1]}")


def test_reference_level_uses_central_window():
    # A curve that's flat at 0 dB in the center and droops hard at the edges;
    # the reference must come from the flat center, not be dragged down by
    # the edges.
    n = 100
    curve = [0.0] * n
    for k in range(10):
        curve[k] = -50.0
        curve[n - 1 - k] = -50.0
    ref = cal.reference_level(curve, window_fraction=0.2)
    check(abs(ref - 0.0) < 1e-6, f"reference should read the flat center, got {ref}")


def test_compute_correction_clamps():
    curve = [0.0, -5.0, -20.0, -50.0]
    correction, clamped_fraction = cal.compute_correction(
        curve, reference_db=0.0, cap_db=15.0)
    check(correction == [0.0, 5.0, 15.0, 15.0],
          f"unexpected correction curve: {correction}")
    # Two of four bins (the -20 and -50 dB ones) needed > 15 dB raw correction.
    check(abs(clamped_fraction - 0.5) < 1e-9,
          f"expected clamped_fraction 0.5, got {clamped_fraction}")


def test_compute_correction_never_negative():
    # A bin ABOVE the reference (e.g. a stray signal the median didn't fully
    # reject) must not produce a negative "correction" that would attenuate
    # a healthy bin.
    curve = [5.0]
    correction, _ = cal.compute_correction(curve, reference_db=0.0, cap_db=15.0)
    check(correction == [0.0], f"correction must clamp to >= 0, got {correction}")


def test_validate_curve_flags_short_capture_count():
    correction = [1.0] * cal.FFT_SIZE
    problems = cal.validate_curve(48, correction, cap_db=15.0,
                                   capture_count=2, min_captures=5)
    check(any("only 2 captures" in p for p in problems),
          f"expected a low-capture-count problem, got {problems}")


def test_validate_curve_flags_wrong_length():
    problems = cal.validate_curve(48, [1.0, 2.0], cap_db=15.0,
                                   capture_count=10, min_captures=5)
    check(any("2 bins" in p for p in problems),
          f"expected a wrong-length problem, got {problems}")


def test_validate_curve_flags_out_of_range():
    correction = [1.0] * cal.FFT_SIZE
    correction[5] = 999.0
    problems = cal.validate_curve(48, correction, cap_db=15.0,
                                   capture_count=10, min_captures=5)
    check(any("bin 5" in p for p in problems),
          f"expected an out-of-range problem for bin 5, got {problems}")


def test_warn_bin_jumps_flags_single_bin_spike():
    correction = [1.0, 1.0, 1.0, 20.0, 1.0, 1.0, 1.0]
    warnings = cal.warn_bin_jumps(48, correction, jump_threshold_db=6.0)
    check(any("bin 3" in w for w in warnings),
          f"expected a spike warning for bin 3, got {warnings}")


def test_warn_bin_jumps_ignores_smooth_ramp():
    correction = [float(i) for i in range(10)]
    warnings = cal.warn_bin_jumps(48, correction, jump_threshold_db=6.0)
    check(warnings == [], f"a smooth ramp should not warn, got {warnings}")


def test_load_captures_accepts_bare_and_wrapped_shapes():
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        bare = {"ddc0RateKsps": 48, "fftSize": 4, "binsDbm": [1.0, 2.0, 3.0, 4.0]}
        wrapped = {"id": "spectrum", "measured": {
            "ddc0RateKsps": 96, "fftSize": 4, "binsDbm": [5.0, 6.0, 7.0, 8.0]}}
        (tmp_path / "a.json").write_text(json.dumps(bare), encoding="utf-8")
        (tmp_path / "b.json").write_text(json.dumps(wrapped), encoding="utf-8")
        captures = cal.load_captures(tmp_path)
        rates = sorted(c["ddc0RateKsps"] for c in captures)
        check(rates == [48, 96], f"expected both shapes to load, got {rates}")


def test_load_captures_skips_incomplete_records():
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        incomplete = {"id": "spectrum", "measured": {"captured": False}}
        (tmp_path / "a.json").write_text(json.dumps(incomplete), encoding="utf-8")
        captures = cal.load_captures(tmp_path)
        check(captures == [], f"an incomplete capture must be skipped, got {captures}")


def test_group_by_rate():
    captures = [
        {"ddc0RateKsps": 48, "binsDbm": [1.0]},
        {"ddc0RateKsps": 48, "binsDbm": [2.0]},
        {"ddc0RateKsps": 96, "binsDbm": [3.0]},
    ]
    grouped = cal.group_by_rate(captures)
    check(len(grouped[48]) == 2 and len(grouped[96]) == 1,
          f"unexpected grouping: {grouped}")


def test_format_table_parseable_as_cpp_array():
    correction = [1.5] * cal.FFT_SIZE
    text = cal.format_table(48, correction)
    check("kDroopTable48Ksps" in text, f"table name missing: {text[:80]}")
    check(text.count("1.5000f") == cal.FFT_SIZE,
          "expected every value literally present")
    check(text.strip().startswith("constexpr DroopCorrectionTable"),
          f"unexpected table declaration: {text[:60]}")


def test_generate_inc_file_includes_all_six_rates_even_if_uncaptured():
    tables = {48: [0.0] * cal.FFT_SIZE}
    text = cal.generate_inc_file(tables, {48: 10}, cap_db=15.0,
                                 reference_window_fraction=0.15)
    for rate in cal.VALID_RATES_KSPS:
        check(f"kDroopTable{rate}Ksps" in text,
              f"missing table for rate {rate} in generated .inc")
    check("DO NOT HAND-EDIT" in text, "missing provenance header")


if __name__ == "__main__":
    test_median_odd()
    test_median_even()
    test_median_power_curve_rejects_a_stray_outlier_capture()
    test_reference_level_uses_central_window()
    test_compute_correction_clamps()
    test_compute_correction_never_negative()
    test_validate_curve_flags_short_capture_count()
    test_validate_curve_flags_wrong_length()
    test_validate_curve_flags_out_of_range()
    test_warn_bin_jumps_flags_single_bin_spike()
    test_warn_bin_jumps_ignores_smooth_ramp()
    test_load_captures_accepts_bare_and_wrapped_shapes()
    test_load_captures_skips_incomplete_records()
    test_group_by_rate()
    test_format_table_parseable_as_cpp_array()
    test_generate_inc_file_includes_all_six_rates_even_if_uncaptured()
    print("anan droop calibration checks passed")
