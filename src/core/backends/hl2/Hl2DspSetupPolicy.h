#pragma once

// How long the DSP-setup phase may run before warning, and before giving up, as
// a pure decision (#5413).
//
// beginDspSetup() hands the WDSP opens to the I/O thread and finishDspSetup() is
// posted back when they finish; MetisClient::start()'s connect watchdog is only
// reached after that, so this phase needs its own bound.
//
// TWO STAGES because a slow first open is legitimate: an uncached WDSP/FFTW
// open measures plans instead of loading them (#5052). Measured cold costs run
// from ~19 s (HERMES §22.3, the outlier) to 98 s on an idle bench, 165 s
// (wdsp_channel_test), ~179-190 s (#4877, including CI), and ~190-220 s for
// whole connects under heavy load. Slower hardware will be slower still.
//
// Hence fail at 600 s: ~3x the largest documented cold cost and still finite.
// Failing a connect that would have succeeded loses the session; a late error
// on a real hang only delays a message the warn line already foreshadows. The
// warn repeats on a fixed cadence until the phase finishes or fails.
//
// These figures cover one receiver. connectRadio() opens one chain unless a
// caller passes an explicit `numRx` connect param (automation/embedders), later
// receivers open outside this phase, and receiverCeiling() caps 384 kHz at 3.
// Further chains at one rate share plans and should be nearly free (expected,
// not measured).
//
// Pure so the timing is testable without a radio, socket or event loop (#5358).

#include <cstdint>

namespace AetherSDR::hl2 {

enum class DspSetupAction {
    None,   // still inside the expected window; say nothing
    Warn,   // slow enough to be worth a log line, NOT a failure
    Fail,   // long enough that the caller deserves an error instead of silence
};

// Default stages. Deliberately far apart: the gap between them is where a
// legitimately slow first open lives — measured at 98.3 s quiet and 188 s under
// load, so the gap is the working case, not the pathological one.
inline constexpr std::int64_t kDspSetupWarnMs = 10'000;
inline constexpr std::int64_t kDspSetupFailMs = 600'000;

// How often to repeat the warning once the phase is past the warn point. Not a
// stage: it changes nothing about what dspSetupAction() decides, only how often
// the caller wakes up to hear the same Warn again.
inline constexpr std::int64_t kDspSetupWarnRepeatMs = 30'000;

inline DspSetupAction dspSetupAction(std::int64_t elapsedMs,
                                     std::int64_t warnMs = kDspSetupWarnMs,
                                     std::int64_t failMs = kDspSetupFailMs)
{
    // Fail is checked FIRST so a misconfigured pair (fail <= warn) still fails
    // rather than warning forever. The ordering is the guard, not an assert:
    // this runs on a timer in a connect path and must not abort a session.
    if (elapsedMs >= failMs) {
        return DspSetupAction::Fail;
    }
    if (elapsedMs >= warnMs) {
        return DspSetupAction::Warn;
    }
    return DspSetupAction::None;
}

// How long to wait before looking again, given that `elapsedMs` has just been
// judged.
//
// Before the warn point this is the exact REMAINDER, not a poll: a connect that
// is going to finish in four seconds costs zero wake-ups. After it, the cadence
// is what keeps the ten-minute window from being silent — but the last wait is
// clamped to the remainder so the fail point is hit exactly rather than
// overshot by up to a repeat interval.
inline std::int64_t dspSetupNextCheckMs(std::int64_t elapsedMs,
                                        std::int64_t warnMs = kDspSetupWarnMs,
                                        std::int64_t failMs = kDspSetupFailMs,
                                        std::int64_t repeatMs = kDspSetupWarnRepeatMs)
{
    if (elapsedMs < warnMs) {
        return warnMs - elapsedMs;
    }
    if (elapsedMs >= failMs) {
        return 0;   // nothing further to wait for
    }
    const std::int64_t remaining = failMs - elapsedMs;
    // A non-positive cadence would arm a zero-delay timer and spin the event
    // loop for the rest of the phase. Fall back to the old single-shot
    // behaviour rather than doing that.
    if (repeatMs <= 0) {
        return remaining;
    }
    return remaining < repeatMs ? remaining : repeatMs;
}

}  // namespace AetherSDR::hl2
