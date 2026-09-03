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
// 50 is unity, because TransmitModel constructs m_micLevel at 50 and nothing
// restores it at startup: a session where the operator never touches the slider
// must leave the modulator exactly at its own 1.0 default. +/-20 dB across the
// travel, linear in dB.
//
// Level 0 is NOT -20 dB — see micSliderToLinear, which handles it as a mute.
// This function is the continuous part of the mapping only.
[[nodiscard]] constexpr double micSliderToGainDb(int level) noexcept
{
    const int clamped = level < 0 ? 0 : (level > 100 ? 100 : level);
    return (static_cast<double>(clamped) - 50.0) * 0.4;
}

// The same slider as the linear multiplier the modulator takes.
//
// Level 0 mutes outright rather than resolving to the -20 dB the line above
// would give it. A slider at the bottom of its travel means off — and a mic
// merely 20 dB down would be hauled back up by the ALC's 40 dB of makeup
// anyway, so without the special case "0" would sound barely different from
// "50", which is the sort of control that teaches an operator to distrust every
// other one on the panel.
//
// SCOPE, because "mic" undersells it: this multiplier is applied to everything
// entering Hl2TxDsp::processAudioBlock, and on a host-modulating backend that
// includes digital-mode and WSPR-beacon audio arriving through submitTxAudio,
// not only voice. For MIC-path audio, above the ALC's hold threshold it is
// very nearly a no-op — the ALC normalizes each block's peak to alcTargetPeak
// and hands the gain straight back. For CLIENT-LEVELED audio (TCI/DAX) the ALC
// may only reduce, never lift (#4796), so below its target there is no handing
// back: this slider is a straight proportional attenuator on that path, and
// TX gain 5 (-18 dB) is a real -18 dB on the air. It stops being straight only
// where it has to — drive a full-scale client through the top of this slider's
// +20 dB and the ALC limits, rather than letting the modulator's hard clamp
// flat-top it, so the last stretch of travel buys reduced headroom rather than
// more power. At 0 neither path transmits: the mic path because silence
// sits below the hold threshold so the ALC declines to lift it, the
// client-leveled path as a plain 0.0x multiply. That is the honest reading of
// a slider at the bottom of its travel on a host modulator — there is one
// modulator and it is off — but it is worth knowing before parking the control
// at 0 between voice sessions.
[[nodiscard]] inline double micSliderToLinear(int level) noexcept
{
    if (level <= 0)
        return 0.0;
    return std::pow(10.0, micSliderToGainDb(level) / 20.0);
}

// ---- Transmit drive: what the wire is asserted at before the operator chooses

// The drive byte to assert while NO drive has been chosen yet.
//
// The setpoint and the value asserted on the wire are different things until
// the operator has actually picked one, and conflating them is what put a
// fresh install on the air at full power.
//
// The trace, on main at 10a847b. connectRadio() claims the start band key
// (Hl2Backend.cpp:1656) but its drive seed below is guarded by
// `if (m_haveRestoredState)`, so on a fresh install m_rfPowerPercent stays at
// its construction default of 100, as does TransmitModel::m_rfPower.
// `emit connected()` (:434) is synchronous, so RadioModel's connect push calls
// setTxPower(100); the change-gate sees 100 == 100 and CORRECTLY declines to
// record it as operator intent -- and applyDrive() writes it anyway, so the
// radio came up at drive 255 with the PA enabled and the first key-up was at
// full power.
//
// applyPerBandStateFor()'s "first visit to a band with no baseline gets 0"
// cannot cover this: it early-returns when the band is already current
// (:4481), and connectRadio claimed the start band before linkUp. So its
// promise -- conservative once per band, rather than hot once per mistake --
// held for every band except the one the session starts on.
//
// WHY NOT SIMPLY SEED THE SETPOINT TO 0. Because that breaks a different
// guarantee, and hl2_state_restore_test catches it: with the setpoint at 0 the
// model-default push of 100 is no longer value-identical, so setTxPower()
// reads it as operator intent and claims the baseline at 100 -- which is
// exactly the defect #4619 fixed (Ozy311: the model-default push made the
// deliberate 0 for unvisited bands unreachable). Keeping the setpoint at its
// default preserves value-identity; gating the WIRE on whether a drive has been
// chosen is what makes the come-up safe. The two properties are independent and
// both are wanted.
[[nodiscard]] constexpr int comeUpDriveByte(bool driveChosen, int percent,
                                            int driveMax) noexcept
{
    if (!driveChosen)
        return 0;
    const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    return clamped * driveMax / 100;
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
