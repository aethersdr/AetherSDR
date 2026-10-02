#pragma once

#include <algorithm>
#include <cmath>

// What the "WtrFall Rate" control means: 1..100, LOW = SLOW, HIGH = FAST, as
// measured on Flex hardware (#3104) and assumed by the slider, time-scale drag
// and SpectrumWidget's time-axis seed. Flex names it `line_duration` (ms in
// FlexLib), but a backend must not pace rows at `value` ms, which runs the
// control backwards (#4606). Two laws, deliberately different:
//   flex*  — the Flex display engine's measured, steeply non-linear law; used
//            to ask a Flex for a cadence and to seed the time axis.
//   local* — our own law for raw-spectrum backends (HL2, demo): linear in rows
//            per second (the Flex curve wastes ~70% of the slider there).

namespace AetherSDR::WaterfallRate {

inline constexpr int kMin = 1;
inline constexpr int kMax = 100;

inline int clampRate(int rate)
{
    return std::clamp(rate, kMin, kMax);
}

// ── Producer A: a Flex ──────────────────────────────────────────────────────

// Measured Flex row cadence per rate value (#3104). Monotonically faster as the
// rate rises, saturating from 93 up where the radio is already delivering rows
// as fast as the panadapter produces frames.
struct Calibration {
    int rate;
    float msPerRow;
};

inline constexpr Calibration kFlexCurve[] = {
    {1, 6000.0f},
    {8, 4000.9f},
    {50, 677.2f},
    {56, 473.2f},
    {67, 223.0f},
    {69, 192.1f},
    {71, 163.9f},
    {75, 120.7f},
    {77, 102.0f},
    {78, 90.5f},
    {79, 88.8f},
    {80, 81.2f},
    {83, 64.2f},
    {90, 46.3f},
    {93, 42.0f},
    {100, 42.0f},
};

inline constexpr int kFlexCurveSize =
    static_cast<int>(sizeof(kFlexCurve) / sizeof(kFlexCurve[0]));

// Rate → the cadence a Flex produces for it. Log-interpolated between measured
// points, because the cadence spans two decades and a linear blend between them
// would misplace everything in between.
inline float flexMsPerRow(int rate)
{
    const int clamped = clampRate(rate);
    if (clamped <= kFlexCurve[0].rate) {
        return kFlexCurve[0].msPerRow;
    }
    for (int i = 1; i < kFlexCurveSize; ++i) {
        const Calibration& lower = kFlexCurve[i - 1];
        const Calibration& upper = kFlexCurve[i];
        if (clamped <= upper.rate) {
            const float fraction = static_cast<float>(clamped - lower.rate)
                / static_cast<float>(upper.rate - lower.rate);
            const float lowerLog = std::log(lower.msPerRow);
            const float upperLog = std::log(upper.msPerRow);
            return std::exp(lowerLog + (upperLog - lowerLog) * fraction);
        }
    }
    return kFlexCurve[kFlexCurveSize - 1].msPerRow;
}

// The inverse: the rate to send a Flex to get roughly this cadence. For callers
// that know the cadence they want — the adaptive throttle knows an fps cap.
inline int flexRateForMsPerRow(float msPerRow)
{
    if (!(msPerRow > 0.0f)) {
        return kMax;
    }
    if (msPerRow >= kFlexCurve[0].msPerRow) {
        return kMin;
    }
    if (msPerRow <= kFlexCurve[kFlexCurveSize - 1].msPerRow) {
        return kMax;
    }
    // The curve is sorted by ascending rate, i.e. DESCENDING msPerRow.
    for (int i = 1; i < kFlexCurveSize; ++i) {
        const Calibration& lower = kFlexCurve[i - 1];
        const Calibration& upper = kFlexCurve[i];
        if (msPerRow >= upper.msPerRow) {
            if (lower.msPerRow <= upper.msPerRow) {
                return clampRate(upper.rate);
            }
            const float fraction =
                (std::log(lower.msPerRow) - std::log(msPerRow))
                / (std::log(lower.msPerRow) - std::log(upper.msPerRow));
            return clampRate(lower.rate
                + static_cast<int>(std::lround(
                    fraction * static_cast<float>(upper.rate - lower.rate))));
        }
    }
    return kMax;
}

// ── Producer B: this host ───────────────────────────────────────────────────

// The endpoints of the local law, in rows per second.
//
// The slow end is 5 s/row rather than something merely slow, because watching a
// slow mode scroll past over minutes is a real use for this control and a purely
// "sensible" floor would delete it. The fast end matches the default panadapter
// frame rate; above it the gate is lifted entirely (see localRowIntervalMs), so
// a pan running at 60 fps is not held down to 25.
inline constexpr float kLocalSlowestRowsPerSec = 0.2f;
inline constexpr float kLocalFastestRowsPerSec = 25.0f;

// Rate → rows per second, LINEAR. Rate 50 is half the speed of rate 100, which
// is the only property an operator can predict without measuring.
inline float localRowsPerSec(int rate)
{
    const float t = static_cast<float>(clampRate(rate) - kMin)
        / static_cast<float>(kMax - kMin);
    return kLocalSlowestRowsPerSec
        + t * (kLocalFastestRowsPerSec - kLocalSlowestRowsPerSec);
}

// Rate → the cadence this host targets for it, in ms.
inline float localMsPerRow(int rate)
{
    return 1000.0f / localRowsPerSec(rate);
}

// The inverse, for a caller that knows the cadence it wants.
inline int localRateForRowsPerSec(float rowsPerSec)
{
    if (!(rowsPerSec > 0.0f)) {
        return kMin;
    }
    const float span = kLocalFastestRowsPerSec - kLocalSlowestRowsPerSec;
    const float t = (rowsPerSec - kLocalSlowestRowsPerSec) / span;
    return clampRate(kMin + static_cast<int>(std::lround(
        t * static_cast<float>(kMax - kMin))));
}

// The pacing interval a self-shaping backend should gate waterfall rows on, in
// whole ms. Zero means "do not gate" — emit one row per spectrum frame.
//
// The top of the control is ungated on purpose. At kMax the operator is asking
// for the fastest the display can go, and the honest ceiling there is the pan's
// own frame rate — which the FFT FPS control already owns. Gating at 40 ms
// instead would both drop rows from a 25 fps stream on rounding and silently cap
// a pan the operator had set to 60.
inline int localRowIntervalMs(int rate)
{
    const int clamped = clampRate(rate);
    if (clamped >= kMax) {
        return 0;
    }
    return std::max(1, static_cast<int>(std::lround(localMsPerRow(clamped))));
}

}  // namespace AetherSDR::WaterfallRate
