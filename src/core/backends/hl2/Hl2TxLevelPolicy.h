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
// or ClientLeveled — the operator's voice, and TCI/DAX client audio, where it
// is the proportional attenuator #4796 left it. On those two paths it is a
// straight proportional control on the air, the same on each, all the way up to
// the ALC's target — the ALC behind it only reduces and has no makeup half left
// to hand the gain back with, so TX gain 5 (-18 dB) is a real -18 dB. It stops
// being straight only where it has to: drive a full-scale source through the
// top of this slider's +40 dB and the ALC limits, rather than letting the
// modulator's hard clamp flat-top it, so the last stretch of travel buys
// reduced headroom rather than more power.
//
// IT DOES NOT REACH ENGINE-GENERATED AUDIO, SO 0 DOES NOT SILENCE A BEACON.
// Hl2TxDsp::processAudioBlock substitutes 1.0 for this multiplier when the
// source is TxAudioSource::EngineGenerated, so the WSPR beacon, the AX.25 modem
// and the RADE waveform go out at the level their generator chose and this
// slider does not move them — at 0 or anywhere else.
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

}  // namespace AetherSDR::hl2
