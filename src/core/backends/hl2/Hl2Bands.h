#pragma once

#include <QString>

namespace AetherSDR {
namespace hl2 {

// Frequency -> stable band key for the HL2's per-band maps (RFC #4603 PR 3:
// per-band TX drive and LNA gain, nigelfenton's review — the PA gain that
// makes 5 W on 80 m makes considerably more on 10 m, so drive is remembered
// per band).
//
// DELIBERATELY a fixed table, not BandPlanManager: these strings are
// PERSISTENCE KEYS inside the radio's OperatingState document, so they must
// be stable across releases, regions, and the operator's band-plan choice.
// Edges are generous (they cover the gaps between ham allocations) so every
// frequency in the HL2's 0.1–38.4 MHz range maps to exactly one key and a
// slightly out-of-band tune stays in its neighborhood's bucket. UI banding
// keeps using the region-aware BandPlanManager; this table never surfaces in
// the UI.
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
