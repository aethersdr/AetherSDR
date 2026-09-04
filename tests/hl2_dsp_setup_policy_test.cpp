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
// takes minutes; with the cache it is seconds. Measured on this bench: 4.1 s
// with the cache, still unfinished at 150 s without it. A single tight timeout
// would therefore fail a connect that is working, so the warn stage exists to
// say "slow, still going" and only the far stage fails.

#include "core/backends/hl2/Hl2DspSetupPolicy.h"

#include <cstdio>

using AetherSDR::hl2::DspSetupAction;
using AetherSDR::hl2::dspSetupAction;
using AetherSDR::hl2::dspSetupNextCheckMs;
using AetherSDR::hl2::kDspSetupFailMs;
using AetherSDR::hl2::kDspSetupWarnMs;

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

    // ---- The schedule: two wake-ups for a whole connect --------------------
    //
    // Returning the REMAINDER rather than a fixed poll interval is what keeps
    // the watchdog to one wake-up at the warn point and one at the fail point,
    // instead of ticking through a ninety-second window.
    check(dspSetupNextCheckMs(0) == kDspSetupWarnMs,
          "from the start, the next look is exactly the warn point");
    check(dspSetupNextCheckMs(kDspSetupWarnMs) == kDspSetupFailMs - kDspSetupWarnMs,
          "from the warn point, the next look is exactly the fail point");
    check(dspSetupNextCheckMs(kDspSetupWarnMs / 2) == kDspSetupWarnMs - kDspSetupWarnMs / 2,
          "an early wake-up reschedules to the warn point, not to a fixed tick");
    check(dspSetupNextCheckMs(kDspSetupFailMs) == 0,
          "past the fail point there is nothing left to wait for");

    // ---- The measured reality this exists to tolerate ----------------------
    //
    // 4.1 s with an FFTW wisdom cache, still running at 150 s without one.
    // The first must be silent; the second must have warned and not yet failed.
    check(dspSetupAction(4'100) == DspSetupAction::None,
          "a cached first open (4.1 s measured) is silent");
    check(dspSetupAction(150'000) == DspSetupAction::Fail,
          "an uncached open still running at 150 s has failed");
    check(dspSetupAction(20'000) == DspSetupAction::Warn,
          "and ~19 s, the documented first-open cost, warns without failing");

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
