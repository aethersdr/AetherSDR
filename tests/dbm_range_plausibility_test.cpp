// The dBm auto-floor ratchet, pinned as arithmetic.
//
// On a backend with no command plane the noise-floor auto-adjust requests a
// range, the request is dropped, no echo comes back, and the loop steps the
// reference level again. Measured against an IC-9700 at a linear 24 dB/s:
//
//     -154 / -64      dropped ("no command plane for this backend")
//     -178 / -88      dropped
//     -202 / -112     REJECTED — first one past dbmRangeLooksPlausible()
//     ...
//     -1882 / -1792   still going, 71 rejections in ~90 s
//
// The rejections are the symptom, not the fault: the run was already 48 dB
// adrift before the first one, and raising the reject floor would only have
// delayed it. What this file pins is the CONSEQUENCE of losing the echo, so
// that a future change which reintroduces an unechoed request has something
// that fails rather than a warning nobody is watching.
//
// dbmRangeLooksPlausible() is the real predicate, from
// core/DbmRangePlausibility.h. This file pins the arithmetic and the shape of
// the runaway; that a fixed-scale backend never arms the loop is pinned in
// icom_family_test via the capability.

#include "core/DbmRangePlausibility.h"
#include "core/backends/RadioCapabilities.h"

#include <QtGlobal>

#include <cmath>
#include <cstdio>

using AetherSDR::RadioCapabilities;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

using AetherSDR::dbmRangeLooksPlausible;

int main()
{
    // The observed ranges, verbatim from the IC-9700 log.
    check(dbmRangeLooksPlausible(-154.0f, -64.0f),
          "-154/-64 is plausible (it was DROPPED, not rejected — already adrift)");
    check(dbmRangeLooksPlausible(-178.0f, -88.0f),
          "-178/-88 is still plausible — the last one before the floor");
    check(!dbmRangeLooksPlausible(-202.0f, -112.0f),
          "-202/-112 is the first rejection");
    check(!dbmRangeLooksPlausible(-1882.0f, -1792.0f),
          "-1882/-1792, 71 steps later, still rejected");

    // The span never changes: the loop slides refLevel and keeps dynamicRange.
    // A drifting span would mean a different fault, so pin the shape too.
    check(std::abs((-64.0f - -154.0f) - 90.0f) < 0.01f, "span is 90 dB at the start");
    check(std::abs((-1792.0f - -1882.0f) - 90.0f) < 0.01f, "span is still 90 dB at the end");

    // A fixed-scale backend's own calibration (ScopeCalibration: floor -140,
    // span 80) MUST be plausible -- if it were not, the re-seed the fix falls
    // back to would itself be rejected and the display would have no range.
    check(dbmRangeLooksPlausible(-140.0f, -60.0f),
          "the Icom's own calibrated range is plausible (the fix's fallback works)");

    // Guard the boundary the ratchet crosses, so a change to kMinAllowedDbm is
    // a deliberate act with a test to update rather than a silent widening.
    check(dbmRangeLooksPlausible(-180.0f, -90.0f), "-180 is exactly on the floor");
    check(!dbmRangeLooksPlausible(-180.01f, -90.0f), "just past the floor is rejected");

    // Degenerate inputs: NaN reached this in the crash-adjacent paths.
    check(!dbmRangeLooksPlausible(std::nanf(""), -60.0f), "NaN min is rejected");
    check(!dbmRangeLooksPlausible(-140.0f, std::nanf("")), "NaN max is rejected");
    check(!dbmRangeLooksPlausible(-60.0f, -140.0f), "inverted range is rejected");

    // The gate defaults to "the radio owns the scale", so adding it changes
    // NOTHING for a Flex or any other existing backend -- only a backend that
    // opts out is affected. Pin that, because getting the default backwards
    // would silently disable the auto-floor everywhere rather than fix one radio.
    check(RadioCapabilities{}.radioOwnsDbmScale,
          "radioOwnsDbmScale defaults TRUE (existing backends keep the auto-floor)");

    if (g_failures == 0)
        std::fprintf(stderr, "dbm_range_plausibility_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
