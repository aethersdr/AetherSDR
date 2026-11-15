#pragma once

// When the ADC-overload warning may be emitted, as a pure decision.
//
// The AD9866 overload flag is a per-frame comparator sample that chatters on a
// strong band, so an edge gate alone is not enough. The first assertion warns
// immediately; the tally of a burst that stops is flushed on a timer, not on the
// next edge (which may never come). Hl2Backend calls these, not a copy.

#include <cstdint>

namespace AetherSDR::hl2 {

// What publishTelemetry should do with the overload counter this update.
struct AdcOverloadWarn {
    bool warn = false;       // emit anything at all?
    bool aggregate = false;  // the "(N times in M ms)" form rather than a bare line
    int  count = 0;          // assertions being reported; meaningful when warn
    bool restartClock = false;
};

// `assertions` counts RISING EDGES of the flag seen since the last flush -- not
// telemetry updates, and not the flag's level. `clockValid` is false before the
// first flush has ever run.
inline AdcOverloadWarn adcOverloadWarn(int assertions,
                                       bool clockValid,
                                       std::int64_t elapsedMs,
                                       std::int64_t intervalMs)
{
    AdcOverloadWarn out;
    if (assertions <= 0) {
        return out;                      // nothing seen; the window keeps running
    }
    // An invalid clock is the first assertion ever: report it now rather than
    // making the operator wait out a window that has not started.
    const bool windowOpen = clockValid && elapsedMs < intervalMs;
    if (windowOpen) {
        return out;                      // suppressed; the count keeps accruing
    }
    out.warn = true;
    out.count = assertions;
    // ONE assertion is a hint and gets the bare line. More than one is the
    // rate, and the rate IS the severity: a flag that sets once is a hint, one
    // that sets on every sample for a minute is a front end being slammed.
    out.aggregate = assertions > 1;
    out.restartClock = true;
    return out;
}

}  // namespace AetherSDR::hl2
