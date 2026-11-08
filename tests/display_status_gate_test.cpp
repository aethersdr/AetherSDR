// Unit tests for applyThrottledDisplayReport() — the adaptive-throttle echo gate
// for radio-authoritative FFT-FPS / waterfall-line-duration status (#4261).
//
// Regression guard for rfoust's blocker #1: a reported setting change WHILE the
// adaptive cap is active must still be applied (so it survives the throttle
// lift), and only the cap's own echo is suppressed.

#include "gui/DisplayStatusGate.h"

#include <cstdio>
#include <set>

using namespace AetherSDR;

namespace {
int g_failed = 0;
int g_total = 0;
void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}
}  // namespace

int main()
{
    // ── Not throttled: any valid value applies; invalid (<= 0) never does. ──
    report("no throttle: valid value applies",
           applyThrottledDisplayReport(false, 0, 25) == true);
    report("no throttle: cap arg is ignored when inactive",
           applyThrottledDisplayReport(false, 25, 25) == true);
    report("value <= 0 never applies (unknown sentinel)",
           applyThrottledDisplayReport(false, 0, 0) == false
               && applyThrottledDisplayReport(true, 15, -1) == false);

    // ── Throttled: suppress ONLY the cap's own echo. ────────────────────────
    report("throttle active: the cap echo is suppressed",
           applyThrottledDisplayReport(true, 15, 15) == false);

    // The regression: a DIFFERENT value reported while capped is a real
    // radio/profile update and must be applied so it isn't lost on lift.
    report("throttle active: a real update (!= cap) IS applied",
           applyThrottledDisplayReport(true, 15, 30) == true);
    report("throttle active: pre-cap value reported again IS applied",
           applyThrottledDisplayReport(true, 15, 25) == true);

    // Line-duration uses the same gate with adaptiveWfRateForCap() as the cap.
    report("line-duration: cap echo suppressed",
           applyThrottledDisplayReport(true, 67, 67) == false);
    report("line-duration: profile change (!= cap) applied",
           applyThrottledDisplayReport(true, 67, 100) == true);

    // ── Every cap this episode sent (#6791). On a Fair/Poor link the cap
    // moves 8 -> 4 -> 8; a status carrying the earlier 8 after the cap moved
    // to 4 is still our echo, not the operator's setting. ─────────────────
    const std::set<int> fpsCapsSent{8, 4};
    report("episode: an earlier tier's echo is suppressed",
           applyThrottledDisplayReport(true, fpsCapsSent, 8) == false);
    report("episode: the current tier's echo is suppressed",
           applyThrottledDisplayReport(true, fpsCapsSent, 4) == false);
    report("episode: the operator's value is applied",
           applyThrottledDisplayReport(true, fpsCapsSent, 25) == true);
    report("episode: nothing is suppressed once the throttle lifts",
           applyThrottledDisplayReport(false, fpsCapsSent, 8) == true);
    report("episode: value <= 0 never applies",
           applyThrottledDisplayReport(true, fpsCapsSent, 0) == false);
    const std::set<int> wfCapsSent{75, 65};
    report("episode: an earlier tier's line-duration echo is suppressed",
           applyThrottledDisplayReport(true, wfCapsSent, 75) == false);
    report("episode: the operator's line duration is applied",
           applyThrottledDisplayReport(true, wfCapsSent, 80) == true);

    if (g_failed == 0) {
        std::printf("\nAll %d display-status-gate tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d display-status-gate tests failed.\n", g_failed, g_total);
    return 1;
}
