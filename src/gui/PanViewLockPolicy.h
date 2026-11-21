#pragma once

#include <algorithm>
#include <cmath>

// Lock view decisions: when a tune may move a locked pan, and where a dragged
// or clicked slice lands. "View" is the hold window, the part of the drawn
// range the radio keeps still under a tuned slice (RadioCapabilities::
// PanViewHold). Contract and callers: docs/architecture/recenter-policy.md.

namespace AetherSDR::PanViewLockPolicy {

enum class TuneKind {
    Incremental,
    ExplicitJump,
};

// Slack on the inclusive edge test. A slice parked exactly on an on-step edge
// must read as on screen, but the edge itself comes from a radio-echoed centre
// and floating-point halving, so it can land a hair inside the slice.
inline constexpr double kEdgeToleranceMhz = 1.0e-6;  // 1 Hz

// A partial hold window ends where the radio's own re-centre test starts (HL2:
// strictly greater than the window). Pulling each edge in by more than the
// tolerance keeps a parked or clicked slice, rounding and slack included,
// strictly inside the radio's window.
inline constexpr double kHoldEdgeGuardMhz = 2.0e-6;  // 2 Hz

// The span a lock can hold: the drawn bandwidth, narrowed to the radio's
// holdable fraction (clamped to [0, 1]) of its published bandwidth, or of the
// drawn span while the radio's is not yet reported. A partial window loses
// kHoldEdgeGuardMhz at each edge.
constexpr double holdBandwidthMhz(double drawnBandwidthMhz,
                                  double radioBandwidthMhz,
                                  double holdableSpanFraction)
{
    if (drawnBandwidthMhz <= 0.0) {
        return 0.0;
    }
    const double fraction = std::clamp(holdableSpanFraction, 0.0, 1.0);
    double held = (radioBandwidthMhz > 0.0 ? radioBandwidthMhz : drawnBandwidthMhz) * fraction;
    if (fraction < 1.0) {
        held = std::max(0.0, held - 2.0 * kHoldEdgeGuardMhz);
    }
    return std::min(drawnBandwidthMhz, held);
}

constexpr bool targetInView(double mhz, double centerMhz, double bandwidthMhz)
{
    if (bandwidthMhz <= 0.0) {
        return false;
    }
    const double halfBw = bandwidthMhz / 2.0;
    return mhz >= centerMhz - halfBw - kEdgeToleranceMhz
        && mhz <= centerMhz + halfBw + kEdgeToleranceMhz;
}

constexpr bool viewMayMove(bool viewLocked, TuneKind kind, bool targetIsInView)
{
    if (!viewLocked) {
        return true;
    }
    if (kind == TuneKind::Incremental) {
        return false;
    }
    return !targetIsInView;
}

// Slice-drag target for a locked view: snapped to the tuning step and held
// inside [center - bw/2, center + bw/2]. The bounds are pulled inward to the
// nearest on-step frequency so snapping can never push the slice back out.
// A span narrower than one step (or no step) falls back to a plain clamp.
inline double clampToViewOnStep(double mhz, double centerMhz,
                                double bandwidthMhz, int stepHz)
{
    if (bandwidthMhz <= 0.0) {
        return mhz;
    }
    const double lo = centerMhz - bandwidthMhz / 2.0;
    const double hi = centerMhz + bandwidthMhz / 2.0;
    if (stepHz <= 0) {
        return std::clamp(mhz, lo, hi);
    }

    // Tolerate binary rounding so an edge that is exactly on-step stays usable.
    constexpr double kGridEpsilon = 1.0e-9;
    const double stepMhz = stepHz / 1.0e6;
    const double gridLo = std::ceil(lo / stepMhz - kGridEpsilon) * stepMhz;
    const double gridHi = std::floor(hi / stepMhz + kGridEpsilon) * stepMhz;
    if (gridLo > gridHi) {
        return std::clamp(mhz, lo, hi);
    }
    const double snapped = std::round(mhz / stepMhz) * stepMhz;
    return std::clamp(snapped, gridLo, gridHi);
}

// Click-to-tune target for a locked view. A click inside the view snaps to
// the nearest on-step frequency that is still inside it (a plain round could
// cross the edge and make the click move the view); a click outside the view
// (HL2's outer margin) snaps normally and is an off-screen jump.
inline double snapClickOnStep(double mhz, double centerMhz,
                              double bandwidthMhz, int stepHz)
{
    if (!targetInView(mhz, centerMhz, bandwidthMhz)) {
        if (stepHz <= 0) {
            return mhz;
        }
        const double stepMhz = stepHz / 1.0e6;
        return std::round(mhz / stepMhz) * stepMhz;
    }
    return clampToViewOnStep(mhz, centerMhz, bandwidthMhz, stepHz);
}

} // namespace AetherSDR::PanViewLockPolicy
