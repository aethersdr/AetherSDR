#pragma once

#include <QtGlobal>

// VITA-49 waterfall-tile frequency decoding.
//
// A tile sub-header carries FrameLowFreq and BinBandwidth as 64-bit integers.
// FlexLib defines both fields as "VitaFrequency" (Hz × 2^20). Decode that
// protocol type directly rather than guessing an encoding from magnitude.
//
// History: the original decoder assumed VitaFrequency, divided, then rejected
// the result as "unreasonable" if it exceeded 1000 MHz — silently re-reading it
// as plain Hz.  That 1 GHz ceiling corrupted every legitimate transverter tile
// above 1 GHz (1296 MHz, 2.3/2.4 GHz, …): a real ~1 GHz VitaFrequency value got
// divided by 1e6 instead of 2^20·1e6, inflating it by 2^20, which pushed every
// waterfall bin outside the panadapter range and rendered the row entirely
// black while the FFT trace stayed correct.  See issues #3449, #1843, #1928,
// #2835 and the failed remap-layer fixes #1845 / #2709 / #2853. A later
// magnitude heuristic removed that upper ceiling but introduced a new one near
// DC: a valid negative tile overhang below about 95 kHz looked like plain Hz,
// inflating its bin bandwidth by 2^20 and mapping the row off-screen (#4412).

namespace AetherSDR::Vita {

struct TileFrequency {
    double lowMhz{0.0};
    double binBwMhz{0.0};
};

// Hz × 2^20 → MHz.
inline constexpr double kVitaFrequencyToMhz = 1048576.0 * 1e6;

inline TileFrequency decodeTileFrequencyMhz(qint64 frameLowRaw, qint64 binBwRaw)
{
    TileFrequency out;
    out.lowMhz   = static_cast<double>(frameLowRaw) / kVitaFrequencyToMhz;
    out.binBwMhz = static_cast<double>(binBwRaw) / kVitaFrequencyToMhz;
    return out;
}

} // namespace AetherSDR::Vita
