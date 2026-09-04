#pragma once

// How long the DSP-setup phase may run before it is worth saying something, and
// before it is worth giving up — as a pure decision.
//
// THE PHASE HAS NO TIMEOUT AT ALL TODAY, and that is the defect (#5413).
// beginDspSetup() hands the WDSP opens to the I/O thread and returns to the
// event loop; finishDspSetup() is posted back when they finish. Between those
// two points nothing in Hl2Backend is watching. The only connect watchdog lives
// in MetisClient::start(), which is reached AFTER this phase, so a stall here is
// outside every guard in the path — the caller sees the same reply a successful
// connect gives and then silence.
//
// TWO STAGES, NOT ONE NUMBER, and the reason is that a slow first open is
// legitimate rather than broken. A machine's first WDSP/FFTW open measures its
// plans instead of loading them, which is genuinely expensive (#5052;
// MetisClient.cpp cites ~19 s), and a single tight timeout would turn a working
// first launch into a failed connect. So: warn early and keep going, fail only
// far out.
//
// A pure function so the timing behaviour is testable without a radio, a socket
// or a running event loop — the layer #5358 asks for.

#include <cstdint>

namespace AetherSDR::hl2 {

enum class DspSetupAction {
    None,   // still inside the expected window; say nothing
    Warn,   // slow enough to be worth a log line, NOT a failure
    Fail,   // long enough that the caller deserves an error instead of silence
};

// Default stages. Deliberately far apart: the gap between them is where a
// legitimately slow first open lives.
inline constexpr std::int64_t kDspSetupWarnMs = 10'000;
inline constexpr std::int64_t kDspSetupFailMs = 90'000;

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
// judged. Returning the REMAINDER rather than a fixed poll keeps the watchdog
// to two wake-ups for a whole connect — one at the warn point, one at the fail
// point — instead of ticking through a ninety-second window.
inline std::int64_t dspSetupNextCheckMs(std::int64_t elapsedMs,
                                        std::int64_t warnMs = kDspSetupWarnMs,
                                        std::int64_t failMs = kDspSetupFailMs)
{
    if (elapsedMs < warnMs) {
        return warnMs - elapsedMs;
    }
    if (elapsedMs < failMs) {
        return failMs - elapsedMs;
    }
    return 0;   // nothing further to wait for
}

}  // namespace AetherSDR::hl2
