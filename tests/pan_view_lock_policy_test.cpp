#include "gui/PanViewLockPolicy.h"

#include <cmath>
#include <cstdio>

// Pins the Lock view contract: a locked pan's visible range holds still for
// every tune except an explicit jump to an off-screen target, and a slice
// drag on a locked pan stops at the edge on an on-step frequency.

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "pan_view_lock_policy_test: %s\n", message);
    return 1;
}

bool nearlyEqual(double a, double b, double epsilon = 1.0e-9)
{
    return std::abs(a - b) <= epsilon;
}

using AetherSDR::PanViewLockPolicy::clampToViewOnStep;
using AetherSDR::PanViewLockPolicy::holdBandwidthMhz;
using AetherSDR::PanViewLockPolicy::snapClickOnStep;
using AetherSDR::PanViewLockPolicy::targetInView;
using AetherSDR::PanViewLockPolicy::TuneKind;
using AetherSDR::PanViewLockPolicy::viewMayMove;

int testTargetInView()
{
    // 14.100 MHz center, 200 kHz span: 14.000 .. 14.200 inclusive.
    if (!targetInView(14.1, 14.1, 0.2)) {
        return fail("center is in view");
    }
    if (!targetInView(14.0, 14.1, 0.2) || !targetInView(14.2, 14.1, 0.2)) {
        return fail("both edges are in view");
    }
    if (targetInView(13.9999, 14.1, 0.2) || targetInView(14.2001, 14.1, 0.2)) {
        return fail("just past either edge is off-screen");
    }
    if (targetInView(14.1, 14.1, 0.0) || targetInView(14.1, 14.1, -1.0)) {
        return fail("an unknown span has nothing in view");
    }
    return 0;
}

int testParkedEdgeStaysInView()
{
    // A slice the locked drag parked on an on-step edge must read as on
    // screen, or re-selecting it would reveal (move) the locked view. The
    // edge comes from halving a radio-echoed span, so it can land a hair
    // inside the parked frequency: 8.104 + 3.764/2 = 9.9859999999999989.
    const double parked = clampToViewOnStep(12.0, 8.104, 3.764, 1000);
    if (!nearlyEqual(parked, 9.986) || !targetInView(parked, 8.104, 3.764)) {
        return fail("a slice parked on the right edge reads as on screen");
    }

    // Sweep Hz-quantized geometries: every parked edge, both sides, is in view.
    for (int centerHz = 7000000; centerHz < 7000000 + 2000; centerHz += 37) {
        for (int bwHz = 1000; bwHz < 400000; bwHz += 4999) {
            const double center = centerHz / 1.0e6;
            const double bw = bwHz / 1.0e6;
            for (int stepHz : {1, 10, 100, 1000}) {
                const double right = clampToViewOnStep(center + bw, center, bw, stepHz);
                const double left = clampToViewOnStep(center - bw, center, bw, stepHz);
                if (!targetInView(right, center, bw) || !targetInView(left, center, bw)) {
                    std::fprintf(stderr, "center=%d bw=%d step=%d\n", centerHz, bwHz, stepHz);
                    return fail("a parked edge read as off-screen");
                }
            }
        }
    }
    return 0;
}

int testHoldBandwidth()
{
    // Whole-span radios hold the drawn span.
    if (!nearlyEqual(holdBandwidthMhz(0.2, 0.2, 1.0), 0.2)) {
        return fail("a whole-span hold is the drawn span");
    }
    // HL2: the NCO recenters past 80% of the half-span, so a 192 kHz span
    // holds the central 153.6 kHz, less the 2 Hz guard at each edge.
    if (!nearlyEqual(holdBandwidthMhz(0.192, 0.192, 0.8), 0.1536 - 4.0e-6)) {
        return fail("HL2 holds only the usable fraction of its span, guarded");
    }
    // A drawn span narrower than the radio's (an edge-taper crop) bounds a
    // whole-span hold, with no guard: there is no radio window edge to stay off.
    if (!nearlyEqual(holdBandwidthMhz(0.18, 0.2, 1.0), 0.18)) {
        return fail("the drawn (cropped) span bounds the hold window");
    }
    // Radio span not yet reported: the fraction scales the drawn span.
    if (!nearlyEqual(holdBandwidthMhz(0.2, 0.0, 0.8), 0.16 - 4.0e-6)) {
        return fail("an unreported radio span falls back to the drawn span");
    }
    // Out-of-range fractions clamp; no drawn span holds nothing.
    if (!nearlyEqual(holdBandwidthMhz(0.2, 0.2, 1.5), 0.2)
        || !nearlyEqual(holdBandwidthMhz(0.2, 0.2, -1.0), 0.0)
        || !nearlyEqual(holdBandwidthMhz(0.0, 0.2, 1.0), 0.0)) {
        return fail("fractions clamp to [0, 1] and a missing span holds nothing");
    }
    return 0;
}

// Hl2Backend::setSliceFrequency() recenters the NCO when
// |tunedHz - ncoHz| > rate/2 * 0.8, computed on the MHz value times 1e6 with no
// rounding. A locked drag or click parked at the hold edge, plus the in-view
// slack, must stay strictly inside that, or the "held" view jumps.
int testHl2HoldEdgeStaysInsideNcoWindow()
{
    constexpr double kFraction = 0.8;  // Hl2Backend::kUsablePassbandFraction
    const int ratesHz[] = {48000, 96000, 192000, 384000};
    const int stepsHz[] = {1, 10, 50, 100, 500, 1000};
    for (int ncoKhz = 1800; ncoKhz <= 30000; ncoKhz += 7) {
        const double centerMhz = ncoKhz / 1000.0;
        for (int rateHz : ratesHz) {
            const double bwMhz = rateHz / 1.0e6;
            const double usableHz = rateHz / 2.0 * kFraction;
            const double hold = holdBandwidthMhz(bwMhz, bwMhz, kFraction);
            for (int stepHz : stepsHz) {
                for (double far : {centerMhz + bwMhz, centerMhz - bwMhz}) {
                    const double parked = clampToViewOnStep(far, centerMhz, hold, stepHz);
                    if (std::abs(parked * 1.0e6 - centerMhz * 1.0e6) > usableHz) {
                        std::fprintf(stderr, "nco=%dkHz rate=%d step=%d parked=%.12f\n",
                                     ncoKhz, rateHz, stepHz, parked);
                        return fail("a parked HL2 edge crosses the NCO recenter test");
                    }
                }
                // The widest frequency the in-view test still accepts.
                const double slackEdge = centerMhz + hold / 2.0 + 0.999e-6;
                if (targetInView(slackEdge, centerMhz, hold)
                    && std::abs(slackEdge * 1.0e6 - centerMhz * 1.0e6) > usableHz) {
                    return fail("the in-view slack reaches past the HL2 NCO window");
                }
            }
        }
    }
    return 0;
}

int testSnapClickOnStep()
{
    // View 0.996 .. 1.046 MHz, 10 kHz step. A click at 1.0455 is on screen but
    // rounds to 1.050 (off screen); it must stay inside at 1.040 instead.
    const double nearEdge = snapClickOnStep(1.0455, 1.021, 0.05, 10000);
    if (!nearlyEqual(nearEdge, 1.04) || !targetInView(nearEdge, 1.021, 0.05)) {
        return fail("a click near the edge snaps inward, not off screen");
    }
    // Same on the left: 0.9962 rounds to 0.990 (off screen) -> 1.000.
    if (!nearlyEqual(snapClickOnStep(0.9962, 1.021, 0.05, 10000), 1.0)) {
        return fail("a click near the left edge snaps inward");
    }
    // Mid-view clicks snap normally.
    if (!nearlyEqual(snapClickOnStep(1.0213, 1.021, 0.05, 10000), 1.02)) {
        return fail("a mid-view click snaps to the nearest step");
    }
    // A click outside the hold window (HL2's outer margin) is an off-screen
    // jump: plain snap, no clamp.
    if (!nearlyEqual(snapClickOnStep(1.0498, 1.021, 0.05, 10000), 1.05)) {
        return fail("a click outside the hold window snaps normally");
    }
    // No step: unchanged.
    if (!nearlyEqual(snapClickOnStep(1.0213, 1.021, 0.05, 0), 1.0213)) {
        return fail("a stepless click is unchanged");
    }
    return 0;
}

int testViewMayMove()
{
    // Unlocked: the existing follow/reveal policy decides everything.
    for (TuneKind kind : {TuneKind::Incremental, TuneKind::ExplicitJump}) {
        if (!viewMayMove(false, kind, true) || !viewMayMove(false, kind, false)) {
            return fail("an unlocked view defers to the existing policy");
        }
    }

    // Locked + incremental: never moves, even when the slice walks off-screen.
    if (viewMayMove(true, TuneKind::Incremental, true)) {
        return fail("locked view must not follow an in-view incremental tune");
    }
    if (viewMayMove(true, TuneKind::Incremental, false)) {
        return fail("locked view must not follow an incremental tune off-screen");
    }

    // Locked + explicit jump: tune in place when on-screen, move when not.
    if (viewMayMove(true, TuneKind::ExplicitJump, true)) {
        return fail("locked view must not hard-center an on-screen jump");
    }
    if (!viewMayMove(true, TuneKind::ExplicitJump, false)) {
        return fail("locked view must follow an explicit jump off-screen");
    }
    return 0;
}

int testClampToViewOnStep()
{
    // Inside the view: plain step snap (1 kHz).
    if (!nearlyEqual(clampToViewOnStep(14.12345, 14.1, 0.2, 1000), 14.123)) {
        return fail("in-view drag snaps to step");
    }

    // Past either edge: held at the on-step edge.
    if (!nearlyEqual(clampToViewOnStep(14.35, 14.1, 0.2, 1000), 14.2)) {
        return fail("drag past the right edge stops at the right edge");
    }
    if (!nearlyEqual(clampToViewOnStep(13.8, 14.1, 0.2, 1000), 14.0)) {
        return fail("drag past the left edge stops at the left edge");
    }

    // Off-step edges pull inward so snapping cannot push the slice out:
    // view 14.0005 .. 14.1995 with a 1 kHz step → 14.001 .. 14.199.
    const double right = clampToViewOnStep(14.5, 14.1, 0.199, 1000);
    if (!nearlyEqual(right, 14.199) || !targetInView(right, 14.1, 0.199)) {
        return fail("off-step right edge pulls in to the last on-step frequency");
    }
    const double left = clampToViewOnStep(13.5, 14.1, 0.199, 1000);
    if (!nearlyEqual(left, 14.001) || !targetInView(left, 14.1, 0.199)) {
        return fail("off-step left edge pulls in to the first on-step frequency");
    }
    // A value that snaps across the edge (14.1996 → 14.200) stays inside.
    if (!targetInView(clampToViewOnStep(14.1996, 14.1, 0.199, 1000), 14.1, 0.199)) {
        return fail("snapping near the edge must not leave the view");
    }

    // No step: plain clamp.
    if (!nearlyEqual(clampToViewOnStep(14.25, 14.1, 0.2, 0), 14.2)) {
        return fail("stepless drag clamps to the edge");
    }
    // No on-step frequency inside the view (14.10475 .. 14.10525 on a
    // 10 kHz grid): plain clamp, never outside the view.
    if (!nearlyEqual(clampToViewOnStep(14.2, 14.105, 0.0005, 10000), 14.10525)) {
        return fail("sub-step span falls back to a plain clamp");
    }
    // Unknown span: leave the frequency alone rather than collapse it.
    if (!nearlyEqual(clampToViewOnStep(14.25, 14.1, 0.0, 1000), 14.25)) {
        return fail("unknown span does not clamp");
    }
    return 0;
}

} // namespace

int main()
{
    if (int rc = testTargetInView()) {
        return rc;
    }
    if (int rc = testParkedEdgeStaysInView()) {
        return rc;
    }
    if (int rc = testHoldBandwidth()) {
        return rc;
    }
    if (int rc = testHl2HoldEdgeStaysInsideNcoWindow()) {
        return rc;
    }
    if (int rc = testSnapClickOnStep()) {
        return rc;
    }
    if (int rc = testViewMayMove()) {
        return rc;
    }
    if (int rc = testClampToViewOnStep()) {
        return rc;
    }
    std::puts("pan_view_lock_policy_test: ok");
    return 0;
}
