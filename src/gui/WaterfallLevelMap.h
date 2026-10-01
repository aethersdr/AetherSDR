#pragma once

#include <algorithm>
#include <cmath>

// The waterfall's value-to-level law, as pure functions.
//
// SpectrumWidget::intensityToWaterfallLevel turns one sample of a waterfall
// row into a 0..1 colour level. It has three black-point sources: the radio's
// per-tile level (HW), this client's noise-floor estimate (SW), and the manual
// Black Level slider (Off). The arithmetic lives here because SpectrumWidget
// links into no test, and the defect this file fixes shipped through every
// green run for that reason.
//
// THE DEFECT. A row arrives in one of two units and the manual branch knew
// only one of them:
//
//   * A Flex sends waterfall TILES: int16(raw) / 128, about 96..120 on HF.
//     Manual black is `160 - level`, 160 down to 60 across the slider.
//   * A radio whose spectrum is computed on this host (HL2, ANAN, RTL-SDR)
//     has no waterfall plane. RadioModel::onBackendSpectrumFrame reuses the
//     pan frame as the row, so the row is dBm: negative numbers.
//
// A threshold of +60..+160 against a row of -148..-111 puts every sample
// below black at every slider position. The SW branch never had the problem,
// because its black point is measured from the row itself and is therefore in
// the row's own unit.
//
// The unit is not guessed from the data. It is the declared capability
// RadioCapabilities::panBinsAbsolute(), which SpectrumWidget already holds as
// m_panBinsAbsolute for the noise-floor gate: true exactly where the bins are
// host-computed absolute levels. A Flex leaves it false and takes the tile law
// unchanged.

namespace AetherSDR::WaterfallLevelMap {

// Manual black point at slider 0, tile-intensity units. The slider runs
// 0..100 at one unit a step, so the black point spans 160 down to 60.
inline constexpr float kManualBlackAtZeroTile = 160.0f;

// Manual black point at slider 0 for a dBm row. One dB a step, so the black
// point spans -60 dBm down to -160 dBm.
//
// CHOSEN HERE, and the reasoning is the whole of its authority. The span has
// to reach below the lowest floor this path produces and stay above ordinary
// signals at the top. On one HL2 on a dummy load the displayed floor ran from
// -111 dBm (LNA -12 dB) to -148 dBm (LNA +40 dB) at the 384 kHz span, 375 Hz a
// bin; the 48 kHz span has bins eight times narrower, 9 dB lower. -160 dBm
// clears that, and -60 dBm at the other end leaves only strong signals lit,
// which is what slider 0 does on a Flex ("well above noise").
inline constexpr float kManualBlackAtZeroDbm = -60.0f;

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
inline float manualBlackThreshold(int blackLevel, bool rowsAreDbm)
{
    const float atZero = rowsAreDbm ? kManualBlackAtZeroDbm
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
    // The row is dBm computed on this host, not Flex tile intensity
    // (RadioCapabilities::panBinsAbsolute()).
    bool  rowsAreDbm{false};
};

// One row sample to a 0..1 colour level.
inline float level(float value, const Params& p)
{
    // Two auto-black paths (a tile arrives as raw_uint16 / 128):
    //  * Radio-authoritative: the radio's per-tile black level is the low/black
    //    point; the white point follows the cubic colour-gain curve
    //    (highThresholdRaw). Reproduces the radio's evenly-levelled floor.
    //  * Fallback (no radio auto-black yet, or auto-black off): the client-side
    //    noise-floor estimate, or the manual black level.
    // The auto-black offset slider biases the black point: 50 = no bias,
    // <50 darker, >50 lighter.
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
        blackThresh = manualBlackThreshold(p.blackLevel, p.rowsAreDbm);
        rangeWidth  = gainRangeWidth(p.colorGain);
    }

    return bound(0.0f, (value - blackThresh) / rangeWidth, 1.0f);
}

}  // namespace AetherSDR::WaterfallLevelMap
