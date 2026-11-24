#pragma once

#include "models/BandPlanManager.h"

#include <QString>

namespace AetherSDR {

// Which band button lights up for a frequency, under the active band plan.
// Shared by the overlay's Band panel and the Band applet so the two always
// agree (#5234).
//
// Broad, non-regulatory grouping windows map a frequency that is already
// proven to lie inside the active BandPlanManager to the matching menu key.
// Actual membership and gaps come only from the active regional plan: a
// frequency outside every segment of the plan, or no plan at all, is GEN.
struct BandPlanLookup {
    const char* bandName;
    double searchLowMhz;
    double searchHighMhz;
};

inline constexpr BandPlanLookup kBandPlanLookup[] = {
    {"2200m", 0.1, 0.2}, {"630m", 0.4, 0.5},
    {"160m", 1.7, 2.1}, {"80m", 3.4, 4.1}, {"60m", 5.2, 5.5},
    {"40m", 6.9, 7.4}, {"30m", 10.0, 10.2}, {"20m", 13.9, 14.5},
    {"17m", 18.0, 18.3}, {"15m", 20.9, 21.6}, {"12m", 24.8, 25.1},
    {"10m", 27.9, 30.0}, {"6m", 49.0, 54.5}, {"4m", 69.0, 71.0},
    {"2m", 143.0, 149.0}, {"1.25m", 221.0, 226.0},
    {"440", 419.0, 451.0}, {"33cm", 901.0, 929.0},
    {"23cm", 1239.0, 1301.0},
};

inline QString activePlanBandForFrequency(const BandPlanManager* manager,
                                          double freqMhz)
{
    if (!manager) {
        return QStringLiteral("GEN");
    }

    bool inActivePlan = false;
    for (const BandPlanManager::Segment& segment : manager->segments()) {
        if (freqMhz >= segment.lowMhz && freqMhz <= segment.highMhz) {
            inActivePlan = true;
            break;
        }
    }
    if (!inActivePlan) {
        return QStringLiteral("GEN");
    }

    for (const BandPlanLookup& lookup : kBandPlanLookup) {
        if (freqMhz >= lookup.searchLowMhz && freqMhz <= lookup.searchHighMhz) {
            return QString::fromLatin1(lookup.bandName);
        }
    }
    return QStringLiteral("GEN");
}

} // namespace AetherSDR
