// The ADC-overload warning's rate limit, as a deterministic decision.
//
// The behaviour is timing-dependent and was previously only reachable by
// running a radio into a strong band for ten seconds — which is why it shipped
// with a bug the compiler could not see: the flush called restart() on a
// QElapsedTimer that the first-assertion path guarantees is invalid, and
// reading elapsed time from a timer that was never started is undefined.
// (#5381 review.)
//
// Hl2Backend evaluates these same functions rather than its own copy, so what
// passes here is what the radio runs
// (core/backends/hl2/Hl2OverloadPolicy.h).

#include "core/backends/hl2/Hl2OverloadPolicy.h"

#include <cstdio>

using AetherSDR::hl2::adcOverloadWarn;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

constexpr std::int64_t kInterval = 10000;   // kAdcOverloadWarnIntervalMs

}  // namespace

int main()
{
    // ---- 1. The FIRST assertion reports immediately ------------------------
    //
    // The clock is invalid before the first flush has ever run. A limiter that
    // made the operator wait out a window here would be silent during exactly
    // the interval when the front end is being slammed and nobody knows yet.
    {
        const auto w = adcOverloadWarn(/*assertions=*/1, /*clockValid=*/false,
                                       /*elapsedMs=*/0, kInterval);
        check(w.warn, "the first assertion warns immediately, invalid clock");
        check(!w.aggregate, "and takes the bare form, not the count form");
        check(w.restartClock, "and starts the window");
    }

    // ---- 2. Chatter inside the window is SUPPRESSED, and accrues -----------
    {
        const auto w = adcOverloadWarn(/*assertions=*/40, /*clockValid=*/true,
                                       /*elapsedMs=*/2500, kInterval);
        check(!w.warn, "chatter inside the window emits nothing");
        check(!w.restartClock, "and does not restart the window");
    }

    // ---- 3. …then AGGREGATES when the window expires -----------------------
    {
        const auto w = adcOverloadWarn(/*assertions=*/133, /*clockValid=*/true,
                                       /*elapsedMs=*/kInterval, kInterval);
        check(w.warn && w.aggregate, "the expired window reports the aggregate");
        check(w.count == 133, "carrying the full count the window swallowed");
    }

    // ---- 4. The window flushes WITHOUT a new assertion ---------------------
    //
    // The decision is made on every telemetry update, not on an edge. A burst
    // that stops must still report its tally: deciding only when the flag next
    // asserts would hold the count until the band goes loud again, which may be
    // hours away or never. Same inputs as case 3 — the point is that reaching
    // this decision requires no new assertion, only the update that carries it.
    {
        const auto w = adcOverloadWarn(/*assertions=*/7, /*clockValid=*/true,
                                       /*elapsedMs=*/kInterval + 5000, kInterval);
        check(w.warn && w.count == 7,
              "a stopped burst still reports its tally once the window expires");
    }

    // ---- 5. An isolated assertion after a quiet interval is immediate ------
    {
        const auto w = adcOverloadWarn(/*assertions=*/1, /*clockValid=*/true,
                                       /*elapsedMs=*/600000, kInterval);
        check(w.warn && !w.aggregate,
              "an isolated overload after a long quiet reports at once, bare");
    }

    // ---- 6. Nothing seen means nothing said, however long the window -------
    //
    // Guards the flush being unconditional on every telemetry update: without
    // this the limiter would warn on a quiet radio forever.
    {
        const auto a = adcOverloadWarn(0, true, kInterval * 100, kInterval);
        const auto b = adcOverloadWarn(0, false, 0, kInterval);
        check(!a.warn && !a.restartClock,
              "no assertions, expired window: silence");
        check(!b.warn && !b.restartClock,
              "no assertions, invalid clock: silence");
    }

    // ---- 7. The boundary is expiry, not strictly-greater --------------------
    {
        const auto below = adcOverloadWarn(5, true, kInterval - 1, kInterval);
        const auto at    = adcOverloadWarn(5, true, kInterval, kInterval);
        check(!below.warn, "one ms before expiry is still suppressed");
        check(at.warn, "at expiry it reports — hasExpired() is inclusive");
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
