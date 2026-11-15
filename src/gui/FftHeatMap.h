#pragma once

// The FFT trace heat-map palette, shared by the main panadapter and the
// mini-pan.
//
// Extracted from SpectrumWidget's paint so the two cannot drift: the mini-pan
// is a magnifier on the main trace, and a second copy of this ramp would mean
// the same signal rendering in two different colours at two zoom levels. One
// definition, one place to retune it.

#include <QColor>

namespace AetherSDR::FftHeatMap {

// Intensity ramp: blue(0) → cyan(0.25) → green(0.5) → yellow(0.75) → red(1.0).
// `t` is 0 at the noise floor, 1 at a full-scale signal.
inline QColor heatColor(float t)
{
    float cr, cg, cb;
    if (t < 0.25f) {
        const float s = t / 0.25f;
        cr = 0.0f; cg = s; cb = 1.0f;
    } else if (t < 0.5f) {
        const float s = (t - 0.25f) / 0.25f;
        cr = 0.0f; cg = 1.0f; cb = 1.0f - s;
    } else if (t < 0.75f) {
        const float s = (t - 0.5f) / 0.25f;
        cr = s; cg = 1.0f; cb = 0.0f;
    } else {
        const float s = (t - 0.75f) / 0.25f;
        cr = 1.0f; cg = 1.0f - s; cb = 0.0f;
    }
    return QColor::fromRgbF(cr, cg, cb);
}

// Base of the per-column fill gradient — the dark blue the heat colour fades
// down to at the bottom of the trace.
inline QColor gradientBase(float fillAlpha)
{
    return QColor(0, 0, 77, static_cast<int>(255 * fillAlpha));
}

// Alpha weighting applied to the heat colour at the top of the fill gradient,
// relative to the operator's FFT Fill alpha.
inline constexpr float kTopAlphaScale = 0.3f;

} // namespace AetherSDR::FftHeatMap
