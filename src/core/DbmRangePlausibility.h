#pragma once

#include <cmath>

// The one definition of "this dBm range could be a real display range".
//
// It was a file-static in MainWindow_Wiring.cpp, where its only job was to stop
// an implausible range leaving for the radio, and tests/dbm_range_plausibility_test
// carried a hand copy because linking that file pulls in the whole GUI. It has a
// second caller now: ClientDisplaySettings refuses to store or restore a range
// that fails it, so a damaged settings row cannot seed the display at -1882 dBm.
// Three users of four constants is where a copy stops being acceptable.

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
