#pragma once

#include <QString>

namespace AetherSDR {
namespace hl2 {

// Frequency -> stable band key for the HL2's per-band maps (TX drive and LNA
// gain; RFC #4603). A fixed table, not BandPlanManager: these strings are
// persistence keys in the OperatingState document and must be stable across
// releases, regions and band-plan choice. Edges are generous so every frequency
// in 0.1-38.4 MHz maps to exactly one key. Never surfaces in the UI.
inline QString bandKeyForHz(double hz)
{
    const double mhz = hz / 1.0e6;
    struct Edge {
        double belowMhz;
        const char* key;
    };
    // Upper edge of each bucket, ascending. The last bucket absorbs the rest
    // of the HL2's tuning range.
    static constexpr Edge kEdges[] = {
        {0.3,   "2200m"},
        {1.0,   "630m"},
        {2.4,   "160m"},
        {4.5,   "80m"},
        {6.0,   "60m"},
        {8.0,   "40m"},
        {11.5,  "30m"},
        {15.5,  "20m"},
        {19.5,  "17m"},
        {22.5,  "15m"},
        {26.0,  "12m"},
        {32.0,  "10m"},
        {40.0,  "8m"},
    };
    for (const Edge& edge : kEdges) {
        if (mhz < edge.belowMhz) {
            return QLatin1String(edge.key);
        }
    }
    return QStringLiteral("8m");
}

} // namespace hl2
} // namespace AetherSDR
