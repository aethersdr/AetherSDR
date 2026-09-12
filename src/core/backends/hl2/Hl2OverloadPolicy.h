#pragma once

// When the ADC-overload warning may be emitted, as a pure decision.
//
// The AD9866's overload flag is a per-frame sample of a level comparator, not
// an event. On a strong band it chatters, so nearly every telemetry sample is a
// rising edge and one message repeats at the full telemetry cadence. An edge
// gate alone is necessary and never sufficient.
//
// Two properties make this worth a seam rather than an inline condition, and
// both are timing-dependent in a way that is otherwise only reachable by
// running a radio for ten seconds:
//
//   * THE FLUSH IS NOT ON AN EDGE. A burst that stops must still report its
//     tally. Deciding only when the flag next asserts would hold the count
//     until the band goes loud again -- which may be hours away, or never.
//   * THE FIRST ASSERTION IS IMMEDIATE. A limiter that made the operator wait
//     out a window before the first warning would be silent during exactly the
//     interval when the front end is being slammed and nobody knows yet.
//
// Hl2Backend evaluates these functions rather than its own copy, so what the
// suite exercises is what the radio runs -- the reasoning Hl2TxLevelPolicy.h
// states, and the same reason it applies here.

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
