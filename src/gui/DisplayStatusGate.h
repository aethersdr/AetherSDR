#pragma once

namespace AetherSDR {

// Adaptive-throttle echo gate for the radio-authoritative FFT-FPS and
// waterfall-line-duration status (#4261).
//
// When the adaptive throttle is active, RadioModel caps those two values and the
// radio echoes the capped value back as status. That echo must NOT overwrite the
// pre-throttle value the widget holds as its restore target — but a *different*
// reported value is a genuine radio/profile update (e.g. a profile load or a
// second client) and must be applied even while throttled, or it is lost when
// the throttle lifts and the stale restore target is pushed back to the radio.
//
// So: apply a reported value iff it is valid (> 0) and it is not exactly the
// value we are currently capping to.  `cappedValue` is the throttle's cap for
// this field (the fps cap, or adaptiveWfMsForCap() for line duration); it is
// only consulted while `throttleActive`.  Pure so it is unit-tested directly.
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
