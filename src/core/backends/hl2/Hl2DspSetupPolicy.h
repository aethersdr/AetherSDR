#pragma once

// Warn/fail bounds for the DSP-setup phase (beginDspSetup() -> finishDspSetup()
// on the I/O thread), which precedes MetisClient::start()'s connect watchdog
// (#5413). An uncached WDSP/FFTW open measures plans (#5052): one receiver's
// cold open measured ~19-220 s, so fail at 600 s (~3x the worst, still finite);
// failing a connect that would succeed costs more than a late error on a hang.
// Figures are for one chain; later receivers open outside this phase.

#include <cstdint>

namespace AetherSDR::hl2 {

enum class DspSetupAction {
    None,   // still inside the expected window; say nothing
    Warn,   // slow enough to be worth a log line, NOT a failure
    Fail,   // long enough that the caller deserves an error instead of silence
};

// Far apart on purpose: a legitimately slow first open lives in the gap.
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
