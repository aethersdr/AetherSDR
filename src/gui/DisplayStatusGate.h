#pragma once

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

}  // namespace AetherSDR
