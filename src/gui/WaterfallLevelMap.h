#pragma once

#include <algorithm>
#include <cmath>

// The waterfall's value-to-level law, as pure functions (SpectrumWidget links
// into no test). A row is either Flex tile intensity (int16 raw / 128, ~96..120)
// or, where the spectrum is computed on this host, the pan's absolute dB bins
// (dBFS under a dBm label, negative); the manual black point must be in the
// row's unit: RadioCapabilities::panBinsAbsolute(), never guessed from the data.

namespace AetherSDR::WaterfallLevelMap {

// Manual black point at slider 0, tile-intensity units. The slider runs
// 0..100 at one unit a step, so the black point spans 160 down to 60.
inline constexpr float kManualBlackAtZeroTile = 160.0f;

// Manual black point at slider 0 for an absolute row, one dB a step: -60 down
// to -160 dB. Chosen against one HL2 (floor about -148 dB at 384 kHz and +40 dB
// LNA, ~9 dB lower at 48 kHz); ANAN and RTL-SDR floors are unmeasured, and no
// RTL-SDR bin goes below -120 dBFS (the clamp in RtlSdrDdc::processSpectrum).
inline constexpr float kManualBlackAtZeroAbsoluteDb = -60.0f;

// qBound's exact comparison order, without Qt: a NaN sample comes out as `lo`
// where std::clamp would hand the NaN through, so the tile path is unchanged
// for every input and not only for the finite ones.
inline float bound(float lo, float value, float hi)
{
    const float capped = (hi < value) ? hi : value;
    return (lo < capped) ? capped : lo;
}

// Colour-gain law shared by the SW and manual branches: the width of the
// black-to-full range, in the row's own unit.
inline float gainRangeWidth(int colorGain)
{
    return std::max(1.0f, 120.0f - static_cast<float>(colorGain) * 0.91f);
}

// Cubic colour-gain curve mapping the radio's black point (low) to a white
// point (high):
//   num  = (100 - colorGain)/100 * cbrt(65535 - low)
//   high = low + num^3        (floored at low + 100)
// colorGain 0 -> full range (dim); 100 -> narrow range (max contrast).
inline float highThresholdRaw(float lowRaw, int colorGain)
{
    const float low = bound(0.0f, lowRaw, 65535.0f);
    const double num = (100.0 - colorGain) / 100.0 * std::cbrt(65535.0 - low);
    double high = low + num * num * num;
    if (high < low + 100.0) {
        high = low + 100.0;
    }
    return static_cast<float>(high);
}

// The manual black point for a slider position, in the unit the row carries.
// Same direction in both units: a HIGHER slider value is a LOWER black point,
// so more of the noise floor is drawn ("Decrease to darken the noise floor",
// the slider's own tooltip).
inline float manualBlackThreshold(int blackLevel, bool rowsAreAbsoluteDb)
{
    const float atZero = rowsAreAbsoluteDb ? kManualBlackAtZeroAbsoluteDb
                                           : kManualBlackAtZeroTile;
    return atZero - static_cast<float>(blackLevel);
}

struct Params {
    bool  autoBlack{true};           // Black Level button is SW or HW
    // HW is in effect: intent AND capability (AutoBlackMode::effectiveRadioSide).
    bool  radioSideAutoBlack{false};
    float radioAutoBlackRaw{0.0f};   // the radio's per-tile level, raw uint16
    float autoBlackThresh{145.0f};   // SW estimate, in the row's own unit
    int   autoBlackOffset{50};       // 0..100, 50 = no bias
    int   blackLevel{15};            // 0..100, manual
    int   colorGain{50};             // 0..100
    // The row is absolute dB computed on this host, not Flex tile intensity
    // (RadioCapabilities::panBinsAbsolute()).
    bool  rowsAreAbsoluteDb{false};
};

// One row sample to a 0..1 colour level.
inline float level(float value, const Params& p)
{
    // Radio-authoritative auto black: the radio's per-tile level is the black
    // point and the white point follows highThresholdRaw. Otherwise the client's
    // noise-floor estimate or the manual level. The offset slider biases the
    // black point: 50 = none, <50 darker, >50 lighter.
    float blackThresh = 0.0f;   // low point  (row unit)
    float rangeWidth = 1.0f;    // high - low (row unit)
    if (p.autoBlack && p.radioSideAutoBlack && p.radioAutoBlackRaw > 0.0f) {
        // Clamp once so the black point, white point, and range all derive from
        // the same low value: the offset can push lowRaw out of [0, 65535].
        const float lowRaw = bound(
            0.0f,
            p.radioAutoBlackRaw
                + static_cast<float>(50 - p.autoBlackOffset) * 0.5f * 128.0f,
            65535.0f);
        const float highRaw = highThresholdRaw(lowRaw, p.colorGain);
        blackThresh = lowRaw / 128.0f;
        rangeWidth  = std::max(1.0f, (highRaw - lowRaw) / 128.0f);
    } else if (p.autoBlack) {
        blackThresh = p.autoBlackThresh
            + static_cast<float>(50 - p.autoBlackOffset) * 0.5f;
        rangeWidth  = gainRangeWidth(p.colorGain);
    } else {
        blackThresh = manualBlackThreshold(p.blackLevel, p.rowsAreAbsoluteDb);
        rangeWidth  = gainRangeWidth(p.colorGain);
    }

    return bound(0.0f, (value - blackThresh) / rangeWidth, 1.0f);
}

}  // namespace AetherSDR::WaterfallLevelMap
