#pragma once

#include <cmath>

// The one definition of "this dBm range could be a real display range": it
// stops an implausible range reaching the radio (MainWindow_Wiring.cpp) and
// keeps a damaged settings row from being stored or restored
// (ClientDisplaySettings).

namespace AetherSDR {

inline bool dbmRangeLooksPlausible(float minDbm, float maxDbm)
{
    constexpr float kMinAllowedDbm = -180.0f;
    constexpr float kMaxAllowedDbm = 80.0f;
    constexpr float kMinRangeDb = 10.0f;
    constexpr float kMaxRangeDb = 180.0f;

    if (!std::isfinite(minDbm) || !std::isfinite(maxDbm)) {
        return false;
    }

    const float rangeDb = maxDbm - minDbm;
    return minDbm >= kMinAllowedDbm
        && maxDbm <= kMaxAllowedDbm
        && rangeDb >= kMinRangeDb
        && rangeDb <= kMaxRangeDb;
}

}  // namespace AetherSDR
