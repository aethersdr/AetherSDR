#pragma once

// The two level calculations on the HL2's transmit path, as pure functions.
//
// Both were live bugs rather than refinements, and both are the kind that a
// running radio reports as "the control does nothing" — which is the hardest
// symptom to act on, because it is indistinguishable from the operator having
// misunderstood the control.
//
// They live in a header, evaluated by Hl2Backend rather than copied into it, so
// the suite exercises the SAME expressions the backend runs. A test against a
// re-typed copy of a mapping proves only that two copies agree; the convention
// error it is meant to catch would sit in both.
//
// See Hl2Backend::setMicGain and Hl2Backend::publishTelemetry for the reasoning
// about WHY each is shaped this way; this header is the arithmetic only.

#include <cmath>
#include <cstddef>
#include <string_view>
#include <utility>

namespace AetherSDR::hl2 {

// ---- Microphone gain -------------------------------------------------------

// The Phone applet's MIC slider (0..100) as dB of gain.
//
// 50 is unity, and stays unity now that Hl2Backend remembers its own radio's
// level across sessions (the txSetpoints extension document, applied in
// pushInitialState): 50 is what a radio with nothing stored comes up on, so
// that session must leave the modulator exactly at its own 1.0 default. The
// persistence changed which sessions arrive here at 50; it did not retire the
// pin, and moving unity would still re-level every install that never asked for
// it.
//
// THE TWO LEGS HAVE DIFFERENT SLOPES, and that asymmetry is the whole reason
// this is not a single multiply. Below 50: -20 dB at 0.4 dB per step, exactly
// as it always was. Above 50: +40 dB at 0.8 dB per step. The upward half was
// widened when Hl2TxDsp's ALC lost its 40 dB of makeup gain — speech sits near
// -32 dBFS and the ALC targets 0.85 (-1.41 dBFS), a ~30 dB shortfall that the
// operator's slider is now the only thing closing, and the old +20 dB left the
// chain 10.6 dB short at maximum travel.
//
// Widening it SYMMETRICALLY would have been tidier and is wrong: it moves unity
// off 50, and the paragraph above is exactly the reason it may not move. So the
// legs meet at 50 with no discontinuity in value (49 = -0.4 dB, 51 = +0.8 dB)
// and a deliberate one in slope. hl2_tx_level_policy_test pins the join, so
// tidying this back to a symmetric mapping fails there rather than on the air.
//
// Level 0 is NOT -20 dB — see micSliderToLinear, which handles it as a mute.
// This function is the continuous part of the mapping only.
[[nodiscard]] constexpr double micSliderToGainDb(int level) noexcept
{
    const int clamped = level < 0 ? 0 : (level > 100 ? 100 : level);
    const double fromUnity = static_cast<double>(clamped) - 50.0;
    return fromUnity <= 0.0 ? fromUnity * 0.4 : fromUnity * 0.8;
}

// The same slider as the linear multiplier the modulator takes.
//
// Level 0 mutes outright rather than resolving to the -20 dB the line above
// would give it. A slider at the bottom of its travel means off, and 0.0x is
// the only reading of that which is not "very quiet".
//
// THE ARGUMENT FOR THE SPECIAL CASE HAS CHANGED, though the behaviour has not.
// It used to be that -20 dB would be hauled straight back up by the ALC's 40 dB
// of makeup, so "0" would have sounded barely different from "50" — a control
// that teaches an operator to distrust every other one on the panel. That
// makeup gain is gone: -20 dB is now a real -20 dB on the air, and "0" would be
// audibly quieter than "50" without the mute. The case survives on the plainer
// ground that the bottom of a travel labelled as a level means off.
//
// SCOPE, and it is narrower than it used to be. This multiplier is applied to
// audio entering Hl2TxDsp::processAudioBlock tagged TxAudioSource::Microphone
// or ClientLeveled — the operator's voice, the AX.25 modem (whose AFSK
// amplitude is a fixed constant this slider is the only way to move), and
// TCI/DAX client audio, where it is the proportional attenuator #4796 left it.
// On those paths it is a straight proportional control on the air, the same on
// each, all the way up to the ALC's target — the ALC behind it only reduces and
// has no makeup half left to hand the gain back with, so TX gain 5 (-18 dB) is a
// real -18 dB. It stops
// being straight only where it has to: drive a full-scale source through the
// top of this slider's +40 dB and the ALC limits, rather than letting the
// modulator's hard clamp flat-top it, so the last stretch of travel buys
// reduced headroom rather than more power.
//
// THAT LAST SENTENCE IS TRUE ONLY BECAUSE REDUCTION IS INSTANTANEOUS, and it is
// worth saying which mechanism holds it up. A smoothed attack does not: at 100x
// the modulator's hard clamp would be reached before the loop got there, and a
// one-shot key-on seed covers only the FIRST reduction of an over, so the next
// loud syllable arrives unprotected — measured on this class at |IQ| 1.5045 with
// 727 clipped samples. Hl2TxDsp's reduction branch simply takes the target
// (`m_alcGain = target`), which is what keeps the widened slider inside the
// modulator's headroom; only the release stays smoothed. hl2_txdsp_test's
// slider-top case asserts it over the WHOLE run rather than the settled tail,
// because the settled tail is precisely the half that cannot see this.
//
// This paragraph described the key-on seed until `5607b565` (#5646) replaced
// it. The seed is gone, `Config::alcAttackSec` with it, and Hl2TxDsp.cpp's own
// comment records why it was rejected.
//
// IT DOES NOT REACH ENGINE-GENERATED AUDIO, SO 0 DOES NOT SILENCE A BEACON.
// Hl2TxDsp::processAudioBlock substitutes 1.0 for this multiplier when the
// source is TxAudioSource::EngineGenerated — the WSPR pump, and nothing else —
// so a beacon goes out at the level its generator chose and this slider does not
// move it, at 0 or anywhere else.
//
// That is deliberate: a microphone control has no business moving, or muting,
// an unattended transmission, and yoking a beacon to the level an operator
// picked for their voice was the defect. But it retires a claim this comment
// used to make — "at 0 nothing transmits, as a plain 0.0x multiply on every
// path" — and that claim was a safety property an operator could have leaned
// on. IT IS NO LONGER TRUE. Parking this control at 0 between voice sessions
// silences the microphone and the TCI/DAX path; it does not silence the
// transmitter. Whatever is generating an unattended transmission is what stops
// it — the beacon's own control, not this one.
[[nodiscard]] inline double micSliderToLinear(int level) noexcept
{
    if (level <= 0)
        return 0.0;
    return std::pow(10.0, micSliderToGainDb(level) / 20.0);
}

// ---- Persisted level migration ---------------------------------------------

// The curve micSliderToGainDb implements, as a number a stored document can
// carry.
//
// Curve 1 was the mapping this radio shipped with while Hl2TxDsp's ALC still
// had 40 dB of makeup gain: -20 dB below unity, +20 dB above, both legs at
// 0.4 dB per step. Curve 2 is what is above — the lower leg unchanged, the
// upper leg widened to +40 dB at 0.8 dB per step, because the makeup gain that
// used to close the ~30 dB speech-to-target shortfall is gone and the slider is
// now the only thing that closes it.
//
// It is a curve number rather than a schema version because it describes what a
// stored NUMBER means, not what keys a document has. A document can gain and
// lose keys without any level in it changing meaning; this changes when the
// meaning of one key does, and only then.
inline constexpr int kMicLevelCurve = 2;

// A slider position stored against curve 1, re-expressed against curve 2 so it
// puts the same gain on the air.
//
// THIS IS NOT A CLAMP AND NOT A PREFERENCE. An operator who parked the slider
// at 80 under curve 1 asked for +12 dB. Under curve 2, 80 means +24 dB — so
// restoring the raw number would hand them 15.849x where they chose 3.981x, on
// the first over after an upgrade, with nothing on the panel to say why. The
// position moves precisely so that the level does not.
//
// Only the upper leg needs it: below 50 both curves are 0.4 dB per step and the
// number already means what it meant. At and below 50 this is the identity,
// including the mute at 0.
//
// The halving is exact in dB and inexact in slider steps — curve 2 has half the
// resolution above unity, so an odd position lands between two steps and rounds
// up, at most 0.4 dB above where it sat. Rounding the other way was the
// alternative and is worse: it rounds toward the unity the operator moved away
// from, and 0.4 dB of extra level is a far smaller surprise on a control whose
// whole upper half is now +40 dB.
[[nodiscard]] constexpr int micLevelFromCurve1(int level) noexcept
{
    if (level <= 50)
        return level;
    const int clamped = level > 100 ? 100 : level;
    // +1 before the integer divide is round-half-up on a non-negative value.
    return 50 + (clamped - 50 + 1) / 2;
}

// ---- Forward-power peak hold -----------------------------------------------

// One step of the transmit forward-power peak hold, in watts.
//
// The HL2's forward power is a single 12-bit conversion from an I2C
// instrumentation ADC with no peak detector and no averaging in the gateware
// (rtl/slow_adc.v), reaching us at 10 Hz. Speech peaks last tens of
// milliseconds, so sampling that envelope at 10 Hz lands on a peak essentially
// never: an SSB reading sat 8-12 dB below PEP while constant-envelope FT8 —
// where every instant IS the peak — read full scale. Both were making the same
// power.
//
// Instant attack, exponential release. What this recovers is NOT an
// instantaneous PEP reading; no filter can recover a peak that was never
// sampled. What it does is accumulate the maximum ACROSS a transmission, so the
// displayed value climbs toward PEP as the over goes on and settles within a
// few dB of it.
//
// `keyed` is a real term, not a guard: unkeyed, the reading must follow the
// instantaneous sample straight down, or a hold outliving the transmission
// keeps re-arming MeterModel's filter and the gauge claims power out of a radio
// that has stopped.
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

// The default TX audio passband for a mode, in hertz.
//
// HERE RATHER THAN IN Hl2Backend.cpp'S ANONYMOUS NAMESPACE for the reason this
// whole header exists, stated at the top of it: a test against a re-typed copy
// of a mapping proves only that two copies agree. hl2_txdsp_test's
// characterisation sweep mirrored these pairs by hand, so widening DIGU's
// window would have left the sweep quietly characterising the OLD passband
// while its comment went on claiming it described what WSJT-X transmits
// through. The sweep's central claim decays silently; that is the failure mode
// worth a move. Caught by aethersdr-agent on #5741, and the same remedy #5725
// applied to kIqSampleRatesHz.
//
// ASCII-UPPERCASING ITS OWN INPUT so the policy owes nothing to Qt and the
// header stays includable by a test that links neither. Hl2Backend keeps a
// QString adapter over this. The two agree for every mode string that exists:
// the literals compared against are ASCII, so a QString::toUpper() beforehand
// can only change characters that could never have matched anyway.
//
// NOT THE WHOLE STORY, and the caller must not treat it as such. The operator
// can override the pair through Hl2Backend::setTxFilter, and
// Hl2Backend::effectiveTxPassband -- which is the function the modulator is
// actually driven from -- returns that override verbatim for USB and LSB. This
// gives the DEFAULT, which is what a mode selects, not what the radio is
// necessarily transmitting through.
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

}  // namespace AetherSDR::hl2
