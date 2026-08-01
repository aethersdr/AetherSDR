// Waterfall rate → row-cadence laws (#4606) — header-only, pure logic.
//
// Two regressions are pinned here.
//
// 1. DIRECTION. The 1..100 "WtrFall Rate" control is a RATE (low slow, high
//    fast), but its wire name is Flex's `line_duration` and FlexLib types that
//    as milliseconds. A consumer that reads the number literally runs the
//    control backwards — on the Hermes-Lite 2 that was rate 1 = 25 fps and rate
//    100 = 10 fps.
//
// 2. USABLE TRAVEL. The first cut of the fix paced this host from the MEASURED
//    FLEX curve, which is steeply log-shaped: rate 50 gave 1.5 rows/s and
//    nothing moved usefully below ~70, wasting most of the slider. Where this
//    host owns the pacing the law is linear in rows/second.

#include "core/WaterfallRate.h"

#include <cstdio>
#include <cmath>

namespace WR = AetherSDR::WaterfallRate;

static int g_total = 0;
static int g_failed = 0;

static void report(const char* name, bool ok)
{
    ++g_total;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", name);
    } else {
        std::printf("ok:   %s\n", name);
    }
}

int main()
{
    // ── Direction — true of BOTH laws, this is the whole first bug. ─────────
    report("flex: rate 1 is the SLOWEST cadence",
           WR::flexMsPerRow(1) > WR::flexMsPerRow(50));
    report("flex: rate 100 is the FASTEST cadence",
           WR::flexMsPerRow(100) < WR::flexMsPerRow(50));
    report("local: rate 1 is the SLOWEST cadence",
           WR::localRowsPerSec(1) < WR::localRowsPerSec(50));
    report("local: rate 100 is the FASTEST cadence",
           WR::localRowsPerSec(100) > WR::localRowsPerSec(50));
    report("a rate read as milliseconds would invert: 1 ms != rate 1",
           WR::flexMsPerRow(1) > 1000.0f && WR::localMsPerRow(1) > 1000.0f);

    bool flexMonotonic = true, localMonotonic = true;
    for (int r = WR::kMin; r < WR::kMax; ++r) {
        if (WR::flexMsPerRow(r + 1) > WR::flexMsPerRow(r)) flexMonotonic = false;
        if (WR::localRowsPerSec(r + 1) <= WR::localRowsPerSec(r)) localMonotonic = false;
    }
    report("flex: cadence never gets slower as the rate rises", flexMonotonic);
    report("local: every step of the slider is strictly faster", localMonotonic);

    // ── Usable travel — the second bug. ────────────────────────────────────
    // Linear in rows/s means the midpoint of the slider is the midpoint of the
    // speed range. Under the Flex curve, rate 50 was 1.5 rows/s out of 25 — 6%
    // of the range at 50% of the travel.
    const float mid = WR::localRowsPerSec(50);
    const float span = WR::kLocalFastestRowsPerSec - WR::kLocalSlowestRowsPerSec;
    const float midFraction = (mid - WR::kLocalSlowestRowsPerSec) / span;
    report("local: slider midpoint is the speed midpoint (+/- 1%)",
           std::fabs(midFraction - 0.5f) < 0.01f);
    report("local: the bottom half of the slider is not wasted",
           WR::localRowsPerSec(50) > 10.0f);
    report("local: motion is visible well below 70",
           WR::localRowsPerSec(25) > 5.0f);
    report("flex curve really is the bunched one (documents the contrast)",
           1000.0f / WR::flexMsPerRow(50) < 2.0f);

    // Equal slider steps are equal speed steps, anywhere on the control.
    const float lowStep = WR::localRowsPerSec(30) - WR::localRowsPerSec(20);
    const float highStep = WR::localRowsPerSec(90) - WR::localRowsPerSec(80);
    report("local: a 10-point move means the same speed change everywhere",
           std::fabs(lowStep - highStep) < 0.01f);

    // ── Endpoints and clamping. ────────────────────────────────────────────
    report("local: the slow end is a multi-second row",
           WR::localMsPerRow(WR::kMin) >= 4000.0f);
    report("local: the fast end matches the default pan frame rate",
           std::fabs(WR::localRowsPerSec(WR::kMax) - 25.0f) < 0.01f);
    report("below-range rate clamps to the slow end",
           WR::localRowsPerSec(-5) == WR::localRowsPerSec(WR::kMin)
               && WR::flexMsPerRow(-5) == WR::flexMsPerRow(WR::kMin));
    report("above-range rate clamps to the fast end",
           WR::localRowsPerSec(4000) == WR::localRowsPerSec(WR::kMax)
               && WR::flexMsPerRow(4000) == WR::flexMsPerRow(WR::kMax));

    // ── The pacing gate a self-shaping backend actually runs. ──────────────
    report("max rate is ungated (one row per spectrum frame)",
           WR::localRowIntervalMs(WR::kMax) == 0);
    report("min rate gates hard (multi-second rows)",
           WR::localRowIntervalMs(WR::kMin) >= 4000);
    report("mid rate gates to a real interval",
           WR::localRowIntervalMs(50) > 0 && WR::localRowIntervalMs(50) < 200);
    // The old HL2 behaviour, stated as a test.
    report("default rate 100 no longer means a 100 ms / 10 fps gate",
           WR::localRowIntervalMs(100) != 100);
    report("rate 1 no longer means a 1 ms / full-frame-rate gate",
           WR::localRowIntervalMs(1) != 1);

    // ── Inverses, used by the adaptive throttle. ───────────────────────────
    report("flex: a tighter fps cap asks for a SLOWER rate",
           WR::flexRateForMsPerRow(1000.0f / 4.0f)
               < WR::flexRateForMsPerRow(1000.0f / 15.0f));
    report("local: a tighter fps cap asks for a SLOWER rate",
           WR::localRateForRowsPerSec(4.0f) < WR::localRateForRowsPerSec(15.0f));
    report("flex: a 4 fps cap does not clamp to the fastest rate",
           WR::flexRateForMsPerRow(1000.0f / 4.0f) < WR::kMax);
    report("local: a 4 fps cap does not clamp to the fastest rate",
           WR::localRateForRowsPerSec(4.0f) < WR::kMax);
    report("very fast request saturates at the top of the control",
           WR::flexRateForMsPerRow(1.0f) == WR::kMax
               && WR::localRateForRowsPerSec(1000.0f) == WR::kMax);
    report("very slow request saturates at the bottom of the control",
           WR::flexRateForMsPerRow(100000.0f) == WR::kMin
               && WR::localRateForRowsPerSec(0.001f) == WR::kMin);
    report("non-positive request is not a divide-by-zero",
           WR::flexRateForMsPerRow(0.0f) == WR::kMax
               && WR::flexRateForMsPerRow(-1.0f) == WR::kMax
               && WR::localRateForRowsPerSec(0.0f) == WR::kMin
               && WR::localRateForRowsPerSec(-1.0f) == WR::kMin);

    // Round-trip: the rate the throttle picks reproduces the cadence it asked
    // for. 20% for the Flex curve (16 measured points, not analytic); the local
    // law is exact to its own rounding.
    bool flexRoundTrip = true, localRoundTrip = true;
    for (int fps : {4, 8, 15, 20}) {
        const float wantMs = 1000.0f / static_cast<float>(fps);
        const float gotMs = WR::flexMsPerRow(WR::flexRateForMsPerRow(wantMs));
        if (std::fabs(gotMs - wantMs) / wantMs > 0.20f) {
            flexRoundTrip = false;
            std::printf("      flex %d fps: wanted %.1f ms, got %.1f ms\n",
                        fps, wantMs, gotMs);
        }
        const float want = static_cast<float>(fps);
        const float got = WR::localRowsPerSec(WR::localRateForRowsPerSec(want));
        if (std::fabs(got - want) / want > 0.03f) {
            localRoundTrip = false;
            std::printf("      local %d fps: wanted %.2f rows/s, got %.2f\n",
                        fps, want, got);
        }
    }
    report("flex: fps cap -> rate -> cadence round-trips within 20%", flexRoundTrip);
    report("local: fps cap -> rate -> cadence round-trips within 3%", localRoundTrip);

    if (g_failed == 0) {
        std::printf("\nAll %d waterfall-rate tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d waterfall-rate tests failed.\n", g_failed, g_total);
    return 1;
}
