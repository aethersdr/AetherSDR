#pragma once

#include <QtGlobal>

// VITA-49 waterfall-tile frequency decoding. FrameLowFreq and BinBandwidth are
// 64-bit FlexLib "VitaFrequency" (Hz × 2^20); decode that type directly, never
// guess the encoding from magnitude, which breaks transverter tiles above 1 GHz
// (#3449) and negative overhang near DC (#4412).

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
