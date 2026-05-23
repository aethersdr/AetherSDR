"""
Shared band definitions for MasterSDR StreamController plugin.
Frequencies from BandDefs.h (defaultFreqMhz converted to Hz).
"""

# (name, default_freq_hz, default_mode)
BANDS = [
    ("160m",  1_900_000, "LSB"),
    ("80m",   3_800_000, "LSB"),
    ("60m",   5_357_000, "USB"),
    ("40m",   7_200_000, "LSB"),
    ("30m",  10_125_000, "DIGU"),
    ("20m",  14_225_000, "USB"),
    ("17m",  18_130_000, "USB"),
    ("15m",  21_300_000, "USB"),
    ("12m",  24_950_000, "USB"),
    ("10m",  28_400_000, "USB"),
    ("6m",   50_150_000, "USB"),
]

BAND_NAMES = [b[0] for b in BANDS]
BAND_FREQS = {b[0]: b[1] for b in BANDS}

# Shared band index for BandUp/BandDown cycling
_band_index = 5  # default to 20m
