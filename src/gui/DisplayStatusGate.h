#pragma once

#include <concepts>

namespace AetherSDR {

// Echo gate for the radio-authoritative FFT-FPS and waterfall line duration
// under the adaptive throttle (#4261). Apply a reported value iff it is > 0
// and not exactly `cappedValue` (the fps cap, or adaptiveWfRateForCap()); the
// echo of our own cap must not overwrite the restore target, but any other
// value is a real update. `cappedValue` is consulted only while throttled.
inline bool applyThrottledDisplayReport(bool throttleActive,
                                        int cappedValue,
                                        int reportedValue)
{
    if (reportedValue <= 0) {
        return false;
    }
    if (throttleActive && reportedValue == cappedValue) {
        return false;  // the adaptive cap's own echo — keep the restore target
    }
    return true;
}

// The same gate against every cap sent since the throttle engaged. The cap
// moves between tiers (e.g. 8 -> 4 -> 8), and the radio can report an earlier
// tier's value after the cap has moved on — in the status that follows a band
// change, for instance. Compared with only the current cap, that stale echo
// read as the operator's setting, became the restore target, and was sent
// back as "the operator's value" on disconnect (#6791). `capsSent` is any set
// with contains(int): QSet<int> in the app, std::set<int> in the test.
template <typename CapSet>
inline bool applyThrottledDisplayReport(bool throttleActive,
                                        const CapSet& capsSent,
                                        int reportedValue)
    requires requires(const CapSet& s) { { s.contains(0) } -> std::convertible_to<bool>; }
{
    if (reportedValue <= 0) {
        return false;
    }
    if (throttleActive && capsSent.contains(reportedValue)) {
        return false;  // an echo of a cap this episode sent
    }
    return true;
}

}  // namespace AetherSDR
