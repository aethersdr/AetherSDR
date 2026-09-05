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
// WHERE "FAR OUT" IS, MEASURED. The first bound here was 90 s, chosen against a
// single observation that an uncached open was "still running at 150 s". It was
// wrong, and wrong in the direction that fails working connects. On an IDLE
// machine — 21 load samples between 3.3 and 4.1 — a cold first open measured
// 98269 ms, against HERMES §22.3's documented 18865 ms
// (streams/hl2-telemetry/runs/d57_bench_quiet_result.txt). So 90 s failed a
// connect that was working, on a quiet machine, every time.
//
// The same series measured that load roughly doubles it: 188128 ms for the open
// at one-minute load 38-40, and four cold CONNECTS at load 31-44 came in at
// 193 / 195 / 214 / 219 s (streams/hl2-telemetry/runs/d57_quiet_connect.py).
// 98.3 s is therefore a FLOOR from one sample on one machine, not a typical
// case — and the cost being measured is FFTW timing candidate plans, which
// varies several-fold across hardware. A laptop or a CI runner will be slower.
//
// AND THE LARGEST DOCUMENTED FIGURE IS NOT OURS. #4877 -- closed, titled "every
// run re-measures 190 s of PATIENT plans" -- reports 178.7 s on an i9-13980HX
// and 188-190 s across four consecutive CI runs. So ~190 s is an EXPECTED cold
// cost in at least one shipped configuration, independently of this bench, and
// the 188.1 s above reproduces it rather than discovering it. Read the range as
// 19 s to 190 s across binaries and platforms before a slower CPU is counted —
// with the low end as the outlier rather than an equal member: three
// independent cold measurements sit between 98 and 190 s (this bench's 98.3 s,
// wdsp_channel_test's 165.1 s, #4877's 178.7 s and its 188-190 s CI runs),
// while HERMES §22.3's 18.9 s and its 22.4 s companion stand alone.
//
// One caveat on treating those as one number: HERMES.md notes that the app's
// plan set and the tests' plan set are different FFTW problems, so #4877's
// figure and a connect are not strictly the same measurement. That cuts toward
// a wider bound, not a narrower one.
//
// WHAT NONE OF THOSE FIGURES COVER: MORE THAN ONE RECEIVER. beginDspSetup()
// opens one chain per receiver — `for (int i = 0; i < actualNumRx; ++i)` — and
// the transmit path opens none, so the cost scales with actualNumRx. Every
// measurement above is a ONE-receiver connect. Plan sets overlap heavily (the
// same bench's second, third and fourth cold opens, at other rates, cost
// 1862 / 1223 / 739 ms after that first 98 s), so receivers 2..N at the same
// rate are probably nearly free — but that is an expectation, not a
// measurement, and the board reports four. If a four-receiver first connect
// ever fails this bound, that is the number to go and measure, not evidence
// that the bound was set too low on the evidence available.
//
// Hence 600 s: an order of magnitude over the quiet floor, ~3x over the largest
// documented cold cost, ~2.7x over the worst measured working connect, and
// still finite. The asymmetry justifies the
// generosity — failing a connect that would have succeeded loses the session,
// while a late error on a true hang only delays a message the operator can
// already see coming from the warn line.
//
// WHICH IS WHY THE WARN REPEATS. A single line at 10 s followed by ten minutes
// of silence is barely better than the unbounded phase this replaces, so after
// the first warning the watchdog re-warns on a fixed cadence until it either
// finishes or fails. The schedule below is what produces that.
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
