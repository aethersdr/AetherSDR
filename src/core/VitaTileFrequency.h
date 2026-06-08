#pragma once

#include <QtGlobal>

// VITA-49 waterfall-tile frequency decoding.
//
// A tile sub-header carries FrameLowFreq and BinBandwidth as 64-bit integers.
// Depending on the radio/firmware these are encoded either as "VitaFrequency"
// (Hz × 2^20) or as plain Hz.  Both fields always share the same encoding.
//
// History: the original decoder assumed VitaFrequency, divided, then rejected
// the result as "unreasonable" if it exceeded 1000 MHz — silently re-reading it
// as plain Hz.  That 1 GHz ceiling corrupted every legitimate transverter tile
// above 1 GHz (1296 MHz, 2.3/2.4 GHz, …): a real ~1 GHz VitaFrequency value got
// divided by 1e6 instead of 2^20·1e6, inflating it by 2^20, which pushed every
// waterfall bin outside the panadapter range and rendered the row entirely
// black while the FFT trace stayed correct.  See issues #3449, #1843, #1928,
// #2835 and the failed remap-layer fixes #1845 / #2709 / #2853.
//
// Disambiguate on the *raw integer magnitude* instead, which has a clean gap:
//   • plain Hz for any real frequency (≤ ~100 GHz) is below 1e11
//   • the smallest VitaFrequency we can see (~95 kHz, well under the 2200 m /
//     135 kHz band edge) is above 1e11
// so a single raw threshold separates the two encodings across the entire
// usable spectrum without imposing any upper frequency limit.

namespace AetherSDR::Vita {

struct TileFrequency {
    double lowMhz{0.0};
    double binBwMhz{0.0};
};

// Below this raw magnitude the value is plain Hz; at or above it, VitaFrequency
// (Hz × 2^20).  1e11 sits in the gap between the two encodings' realistic
// ranges (see header comment).
inline constexpr qint64 kVitaFrequencyRawThreshold = 100000000000LL; // 1e11

// Hz × 2^20 → MHz.
inline constexpr double kVitaFrequencyToMhz = 1048576.0 * 1e6;

inline TileFrequency decodeTileFrequencyMhz(qint64 frameLowRaw, qint64 binBwRaw)
{
    // The encoding is decided from FrameLowFreq (the large, reliable value) and
    // applied to BinBandwidth too, since both fields share one encoding.
    const bool isVitaFrequency = qAbs(frameLowRaw) >= kVitaFrequencyRawThreshold;
    const double scale = isVitaFrequency ? kVitaFrequencyToMhz : 1e6;

    TileFrequency out;
    out.lowMhz   = static_cast<double>(frameLowRaw) / scale;
    out.binBwMhz = static_cast<double>(binBwRaw) / scale;
    return out;
}

} // namespace AetherSDR::Vita
