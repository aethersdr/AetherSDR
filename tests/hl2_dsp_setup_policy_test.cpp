// When the DSP-setup phase is worth a warning, and when it is worth failing.
//
// The phase this governs had NO timeout at all (#5413): beginDspSetup() hands
// the WDSP opens to the I/O thread and returns, finishDspSetup() is posted back
// when they finish, and between those points nothing was watching. The only
// connect watchdog lives in MetisClient::start(), which is reached after this
// phase — so a stall here was outside every guard in the path and a caller got
// the same reply a successful connect gives, then silence.
//
// Timing behaviour is otherwise reachable only by running a radio, which is why
// it is a pure function here — the layer #5358 asks for.
//
// THE TWO STAGES ARE THE POINT. A first WDSP open on a machine with no FFTW
// wisdom cache measures 35 plans per channel with FFTW_PATIENT and legitimately
// takes minutes; with the cache it is seconds. A single tight timeout would
// therefore fail a connect that is working, so the warn stage exists to say
// "slow, still going" and only the far stage fails.
//
// THE NUMBERS BELOW ARE MEASUREMENTS, AND THEY MOVED THE BOUND. The first
// version of this file asserted that an open still running at 150 s "has
// failed", against a 90 s bound. Both were wrong: a cold first open on an IDLE
// machine measures 98269 ms
// (streams/hl2-telemetry/runs/d57_bench_quiet_result.txt, 21 load samples
// between 3.3 and 4.1), so 90 s failed a working connect every time on a quiet
// machine, and 150 s is inside the working range rather than outside it. Under
// load the same open is 188128 ms and whole cold connects are 193-219 s
// (streams/hl2-telemetry/runs/d57_quiet_connect.py). And the largest figure is
// not this bench's at all: #4877, closed, reports 178.7 s on an i9-13980HX and
// 188-190 s across four CI runs as the EXPECTED cold cost. Hence the checks
// here run against 600 s.

#include "core/backends/hl2/Hl2DspSetupPolicy.h"

#include <cstdio>

using AetherSDR::hl2::DspSetupAction;
using AetherSDR::hl2::dspSetupAction;
using AetherSDR::hl2::dspSetupNextCheckMs;
using AetherSDR::hl2::kDspSetupFailMs;
using AetherSDR::hl2::kDspSetupWarnMs;
using AetherSDR::hl2::kDspSetupWarnRepeatMs;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

int main()
{
    // ---- Inside the expected window, say nothing --------------------------
    check(dspSetupAction(0) == DspSetupAction::None,
          "a phase that has just started is not reported");
    check(dspSetupAction(kDspSetupWarnMs - 1) == DspSetupAction::None,
          "one ms before the warn point is still silent");

    // ---- Slow, but NOT a failure ------------------------------------------
    //
    // This is the stage that keeps a working first open from being killed.
    check(dspSetupAction(kDspSetupWarnMs) == DspSetupAction::Warn,
          "at the warn point it warns — the boundary is inclusive");
    check(dspSetupAction(kDspSetupFailMs - 1) == DspSetupAction::Warn,
          "one ms before the fail point it is STILL only a warning");

    // ---- Far out, fail ----------------------------------------------------
    check(dspSetupAction(kDspSetupFailMs) == DspSetupAction::Fail,
          "at the fail point it fails");
    check(dspSetupAction(kDspSetupFailMs * 10) == DspSetupAction::Fail,
          "and stays failed however long it runs");

    // ---- A misconfigured pair fails rather than warning forever -----------
    //
    // The ordering inside the function is the guard. This runs on a timer in a
    // connect path, so an assert would abort a session; getting the answer
    // wrong quietly would leave the phase unbounded again, which is the very
    // defect.
    check(dspSetupAction(5000, /*warnMs=*/9000, /*failMs=*/1000) == DspSetupAction::Fail,
          "fail <= warn still fails, rather than warning forever");

    // ---- The schedule ------------------------------------------------------
    //
    // Before the warn point, the exact REMAINDER: a connect that finishes in
    // four seconds costs no wake-ups at all. After it, a fixed cadence, because
    // the window it is covering is ten minutes wide and one line at 10 s
    // followed by silence is barely better than no watchdog.
    check(dspSetupNextCheckMs(0) == kDspSetupWarnMs,
          "from the start, the next look is exactly the warn point");
    check(dspSetupNextCheckMs(kDspSetupWarnMs / 2) == kDspSetupWarnMs - kDspSetupWarnMs / 2,
          "an early wake-up reschedules to the warn point, not to a fixed tick");
    check(dspSetupNextCheckMs(kDspSetupWarnMs) == kDspSetupWarnRepeatMs,
          "from the warn point, it re-warns on the cadence");
    check(dspSetupNextCheckMs(kDspSetupWarnMs + kDspSetupWarnRepeatMs) == kDspSetupWarnRepeatMs,
          "and keeps re-warning on it");
    check(dspSetupNextCheckMs(kDspSetupFailMs) == 0,
          "past the fail point there is nothing left to wait for");

    // The last wait is the remainder, not the cadence — otherwise the fail
    // point is overshot by up to one interval and the bound is not the bound.
    check(dspSetupNextCheckMs(kDspSetupFailMs - 1) == 1,
          "the final wait lands exactly on the fail point, not past it");
    check(dspSetupNextCheckMs(kDspSetupFailMs - kDspSetupWarnRepeatMs) == kDspSetupWarnRepeatMs,
          "one full cadence out, the cadence is still exactly right");

    // A zero or negative cadence must not arm a zero-delay timer and spin the
    // event loop for the rest of the phase. It falls back to a single wait.
    check(dspSetupNextCheckMs(kDspSetupWarnMs, kDspSetupWarnMs, kDspSetupFailMs,
                              /*repeatMs=*/0) == kDspSetupFailMs - kDspSetupWarnMs,
          "a zero cadence degrades to one long wait, not to a spin");

    // ---- The measured reality this exists to tolerate ----------------------
    //
    // These are the numbers that moved the bound from 90 s to 600 s. A cold
    // first open on a QUIET machine is 98.3 s; under load the same open is
    // 188.1 s and a whole cold connect is 193-219 s. Every one of those is a
    // connect that WORKED, so every one of them must warn rather than fail.
    check(dspSetupAction(4'100) == DspSetupAction::None,
          "a cached first open (4.1 s measured) is silent");
    check(dspSetupAction(20'000) == DspSetupAction::Warn,
          "~19 s, HERMES 22.3's documented first-open cost, warns without failing");
    check(dspSetupAction(98'269) == DspSetupAction::Warn,
          "98.3 s, a cold open measured on an IDLE machine, must NOT fail");
    check(dspSetupAction(150'000) == DspSetupAction::Warn,
          "150 s, the observation the 90 s bound was built on, is inside the working range");
    check(dspSetupAction(188'128) == DspSetupAction::Warn,
          "188.1 s, the same open under load, must NOT fail");
    check(dspSetupAction(219'000) == DspSetupAction::Warn,
          "219 s, the slowest cold CONNECT measured, must NOT fail");
    check(dspSetupAction(190'000) == DspSetupAction::Warn,
          "190 s, #4877's expected cold cost on an i9 and in CI, must NOT fail");
    check(dspSetupAction(600'000) == DspSetupAction::Fail,
          "and the phase is still bounded — 600 s fails");

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
