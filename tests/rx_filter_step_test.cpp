// The filter-preset index arithmetic, in isolation.
//
// This exists because the same class of bug appeared twice in one change: a
// search over one list, an index into another. RxApplet holds the operator's
// configurable preset list AND, on a radio that declares a fixed set, the
// radio's — and where the two differ in length, mixing them is an out-of-range
// access rather than merely a wrong answer.
//
// No GUI: RxApplet needs half the app to construct, which is exactly why the
// arithmetic was untested and stayed wrong.

#include "gui/FilterStepMath.h"

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main()
{
    // The IC-705's three IF filters, and a typical operator preset list. The
    // lengths differ, which is the whole hazard.
    const QVector<int> radio{1800, 2400, 3000};
    const QVector<int> operatorList{1800, 2100, 2400, 2700, 2900, 3300};

    // WIDEN AT THE TOP OF THE RADIO LIST. Stepping up from 3000 has nowhere to
    // go and must stay put — the pre-fix code clamped against the operator list
    // instead and returned index 3, applying 2700: a NARROWER filter, on a
    // width this radio cannot reach, in response to a widen request.
    check(steppedFilterWidthIndex(radio, 3000, +1) < 0,
          "widening at the top of the radio's list is a no-op, not a jump into another array");
    check(radio[nearestFilterWidthIndex(radio, 3000)] == 3000,
          "and 3000 Hz really is the top of that list");

    // THE FM CRASH. In FM the operator list is empty while a radio list may
    // still be in force. The old code called std::clamp(idx +/- 1, 0, -1) —
    // lo > hi, a precondition violation — and then indexed an empty QVector.
    // Reachable from the filter-step shortcut and the controller rotary,
    // neither of which is gated on the filter row being visible.
    const QVector<int> empty;
    check(steppedFilterWidthIndex(empty, 2400, +1) < 0, "an empty list steps nowhere, up");
    check(steppedFilterWidthIndex(empty, 2400, -1) < 0, "an empty list steps nowhere, down");
    check(nearestFilterWidthIndex(empty, 2400) < 0,
          "and reports NO nearest index rather than 0, which would index nothing");

    // Ordinary stepping, both directions, within one list.
    check(steppedFilterWidthIndex(radio, 1800, +1) == 1, "1800 steps up to 2400");
    check(steppedFilterWidthIndex(radio, 3000, -1) == 1, "3000 steps down to 2400");
    check(steppedFilterWidthIndex(radio, 1800, -1) < 0, "and stops at the bottom");

    // A width BETWEEN presets snaps rather than refusing: the operator is at
    // 2500 Hz after a manual drag, and a step up should reach 3000.
    check(steppedFilterWidthIndex(radio, 2500, +1) == 2,
          "an off-preset width steps to the next preset, not nowhere");

    // The operator's own list still behaves, so the fix did not narrow it.
    check(steppedFilterWidthIndex(operatorList, 2400, +1) == 3, "operator list steps up");
    check(steppedFilterWidthIndex(operatorList, 3300, +1) < 0, "operator list stops at its top");

    // Direction 0 is not a step.
    check(steppedFilterWidthIndex(radio, 2400, 0) < 0, "no direction is no step");

    if (g_failures == 0)
        std::printf("rx_filter_step_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
