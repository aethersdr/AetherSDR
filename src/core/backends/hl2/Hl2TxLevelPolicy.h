#pragma once

// The HL2 transmit-path level calculations, as pure functions. Hl2Backend
// calls these directly so the tests exercise the same expressions it runs.
// Reasoning lives at Hl2Backend::setMicGain and Hl2Backend::publishTelemetry.

#include <cmath>
#include <cstddef>
#include <string_view>
#include <utility>

namespace AetherSDR::hl2 {

// ---- Microphone gain -------------------------------------------------------

// The Phone applet's MIC slider (0..100) as dB of gain. 50 is unity (a radio
// with nothing stored leaves the modulator at 1.0), so it must not move.
// Below 50: 0.4 dB/step to -20 dB. Above 50: 0.8 dB/step to +40 dB, because
// the ALC has no makeup gain and speech (~-32 dBFS) is ~30 dB short of its
// 0.85 target. The join at 50 is continuous in value, not slope;
// hl2_tx_level_policy_test pins it. Level 0 is a mute (micSliderToLinear).
[[nodiscard]] constexpr double micSliderToGainDb(int level) noexcept
{
    const int clamped = level < 0 ? 0 : (level > 100 ? 100 : level);
    const double fromUnity = static_cast<double>(clamped) - 50.0;
    return fromUnity <= 0.0 ? fromUnity * 0.4 : fromUnity * 0.8;
}

// The slider as the modulator's linear multiplier; level 0 mutes. Applies to
// Microphone and ClientLeveled audio (voice, AX.25, TCI/DAX), not the WSPR
// pump (Hl2TxDsp substitutes 1.0), so 0 does not silence a beacon. The ALC
// only reduces, and reduction is instantaneous (`m_alcGain = target`), so a
// full-scale source at the slider top is limited by the ALC, not flat-topped
// by the modulator clamp; hl2_txdsp_test's slider-top case asserts it.
[[nodiscard]] inline double micSliderToLinear(int level) noexcept
{
    if (level <= 0)
        return 0.0;
    return std::pow(10.0, micSliderToGainDb(level) / 20.0);
}

// ---- Persisted level migration ---------------------------------------------

// The curve micSliderToGainDb implements, stored with persisted levels.
// Curve 1: +/-20 dB at 0.4 dB/step. Curve 2: lower leg unchanged, upper leg
// +40 dB at 0.8 dB/step. A curve number (what a stored number means), not a
// schema version (what keys exist).
inline constexpr int kMicLevelCurve = 2;

// A curve-1 slider position re-expressed on curve 2 so it puts the same gain on
// the air (curve-1 80 = +12 dB; raw 80 on curve 2 would be +24 dB). Identity
// at and below 50. Odd positions round up, at most 0.4 dB above the old level.
[[nodiscard]] constexpr int micLevelFromCurve1(int level) noexcept
{
    if (level <= 50)
        return level;
    const int clamped = level > 100 ? 100 : level;
    // +1 before the integer divide is round-half-up on a non-negative value.
    return 50 + (clamped - 50 + 1) / 2;
}

// ---- Forward-power peak hold -----------------------------------------------

// One step of the TX forward-power peak hold, in watts. Forward power is one
// unaveraged 12-bit I2C ADC sample (rtl/slow_adc.v), re-sampled every other EP6
// response (control.v:261); the input is each publish window's maximum. Instant
// attack and exponential release carry the max across an over. Unkeyed, the
// reading follows the instant sample so the gauge drops when TX stops.
[[nodiscard]] constexpr double fwdPeakHoldStep(double previousPeakW,
                                               double instantW,
                                               bool keyed,
                                               double releaseAlpha) noexcept
{
    if (!keyed)
        return instantW;
    if (instantW >= previousPeakW)
        return instantW;
    return previousPeakW + releaseAlpha * (instantW - previousPeakW);
}

// ---- Transmit passband -----------------------------------------------------

// The default TX audio passband for a mode, in Hz. Lives here so
// hl2_txdsp_test's sweep uses these pairs rather than a copy. ASCII-uppercases
// its input to stay Qt-free (Hl2Backend wraps it for QString). This is the
// mode default only: Hl2Backend::effectiveTxPassband returns the operator's
// setTxFilter override instead for USB/LSB.
[[nodiscard]] constexpr std::pair<int, int>
defaultTxPassbandForModeName(std::string_view mode) noexcept
{
    constexpr auto eq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char c = a[i];
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
            if (c != b[i])
                return false;
        }
        return true;
    };
    if (eq(mode, "DIGU") || eq(mode, "DIGL")) return {150, 3000};
    if (eq(mode, "CWU") || eq(mode, "CW") || eq(mode, "CWL")) return {300, 900};
    if (eq(mode, "AM") || eq(mode, "SAM") || eq(mode, "DSB")) return {100, 3000};
    if (eq(mode, "FM") || eq(mode, "NFM")) return {100, 3000};
    return {300, 2700};   // USB/LSB and anything else: the voice default
}

// TXA's AM carrier level and FM peak deviation: create_txa()'s own values.
// 0.5 makes full-scale audio exactly 100 % modulation at the PEP of a
// full-scale SSB peak (carrier is a quarter of PEP). 5000 Hz matches the RX FM
// detector; it is the deviation for unit audio after TXA's pre-emphasis, so
// speech deviates less. No operator control reaches either value.
inline constexpr double kTxAmCarrierLevel = 0.5;
inline constexpr double kTxFmDeviationHz = 5000.0;

}  // namespace AetherSDR::hl2
