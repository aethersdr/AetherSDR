#pragma once

// The HL2's two ADC level readings, paired — as a pure decision.
//
// HERMES.md §13 item 16, A3 §7: "the single most useful diagnostic pairing on
// the HL2". There are two answers to "how hard is the converter being driven",
// they are measured at different points, and THEY DISAGREE BY DESIGN:
//
//   * PRE-DDC — the HL2 gateware's clip indicator (EP6 RADDR 0x00, DATA bit
//     24, decoded in MetisProtocol's Hl2Telemetry::apply). It watches the
//     converter's input, so it sees the WHOLE 0-38.4 MHz the converter sees:
//     a broadcast station 20 MHz away can set it without ever appearing in
//     the slice the operator is listening to.
//
//     IT IS NOT A LEVEL COMPARATOR, and an earlier version of this comment
//     said it was. The gateware builds the bit as `(&clip_cnt)` — an
//     AND-reduction over its clip counter; the slot layout is transcribed
//     from control.v at 883a338 beside the decode in MetisProtocol.cpp — so
//     it asserts only once that counter is SATURATED, every bit set, not when
//     a level is crossed. `clip_cnt` is two bits wide, which makes saturation
//     AT LEAST THREE CLIP EVENTS inside one reporting interval. That width
//     comes from control.v itself, which this tree does NOT carry — it is
//     transcribed here, not re-derived, and the counter's reset cadence has
//     not been established at all. Two things follow that the labels must
//     respect: a signal that clips occasionally sets nothing, so a clear bit
//     is NOT "the converter is comfortable"; and the bit is not the counter.
//     HERMES.md §12.5 keeps "the HL2's clip counter and overload bit" as two
//     things, and §13 row 14 — still open — is where the count as a count
//     belongs.
//   * POST-DDC — WDSP's RXA_ADC_PK for the receive chain (third_party/wdsp
//     RXA.c: the adcmeter runs first in xrxa, on midbuff, ahead of nbp0). It
//     sees ONE slice, after the gateware's DDC and after WDSP's input
//     half-band decimation to the 48 kHz DSP rate.
//
// The disagreement is the whole point. This lab has measured exactly the case
// the oracle describes — a quiet slice while the converter saturates on
// something far outside it — and it is why the clip flag alone was the wrong
// driver for a gain decision. Read together the two numbers say WHERE the
// energy is; read apart, neither can.
//
// NOTHING HERE IS CALIBRATED, and the two sides are not even on a common
// scale. The slice peak is dB relative to WIRE full scale; the DDC between the
// two measurement points carries a processing gain nobody here has quantified;
// and Hl2DbReference::fullScaleDbm defaults to 0.0 with isCalibrated() false,
// so no reading in this file is antenna-referred. What survives the missing
// calibration is the PAIRING: "the converter is overloading while this slice
// sits 40 dB below full scale" is true, useful, and contains no absolute
// reference at all. Labels must claim exactly that much and no more.
//
// DISPLAY ONLY. IRadioBackend.h's health contract binds: "Purely for display —
// nothing in the app makes a decision from it." Nothing reads this verdict
// back; no gain, no AGC, no drive, no filter follows from it.
//
// A seam rather than an inline condition, for Hl2OverloadPolicy.h's reason:
// most branches below are otherwise reachable only by driving a real converter
// into a real overload, which is not a thing a test suite can arrange. The
// exception is the staleness gate at kSliceStaleMs, which needs no overload at
// all — and that is the branch that was WRONG before the gate existed, so the
// seam has now paid for itself once.

#include <cmath>
#include <cstdint>

namespace AetherSDR::hl2 {

// Below this, a WDSP meter reading is not a measurement.
//
// Two sentinels land here. WdspChannel::meter() returns -300.0 for a transmit
// channel, which has no RXA meters at all; WDSP's own meter.c writes -400.0
// for a stage that is not running, and its 10*log10(peak + 1e-40) reaches the
// same floor for an input that is identically zero. A real HF slice always
// carries noise, so none of these is a level the antenna can produce, and
// reporting any of them as "the slice is at -400 dBFS" would dress a missing
// reading up as a measurement.
inline constexpr double kAdcMeterSilentDbfs = -200.0;

// How close to wire full scale the post-DDC slice must sit to count as "hot".
//
// A DISPLAY BOUNDARY, NOT A CALIBRATED ONE — there is no calibrated boundary
// available to have, for the reason the file comment gives: the two sides of
// the pairing do not share a scale. It exists only so the readout can say
// which of four things is happening in words instead of leaving an operator to
// compare a dB figure against a boolean in their head. Nothing acts on it.
inline constexpr double kSliceHotHeadroomDb = 3.0;

// How old the post-DDC slice reading may be and still be paired with a live
// overload flag.
//
// THE TWO SIDES OF THE PAIRING DO NOT STOP AT THE SAME TIME. Hl2RxDsp holds
// the slice peak at its last receive value for the whole of a transmission —
// it has to, because the chain is clocked with silence there and the meter
// would otherwise decay to WDSP's floor — while Hl2Telemetry::adcOverload
// keeps updating, because EP6 responses ride the same datagrams as the IQ and
// RX streaming continues through TX on the HL2. Paired with no freshness test,
// a converter overload during transmit — and on an HL2 the transmitter is on
// the same port as the receiver, so that is a routine reading — comes out as
// "the signal doing it is elsewhere in 0-38.4 MHz" against a frozen quiet
// slice. That is the exact opposite of what is happening, and it sends the
// operator to the attenuator to fix their own PTT. A held NUMBER with an age
// beside it is honest; a held SENTENCE asserting causation is not.
//
// CHOSEN, NOT MEASURED. What the value is worth is the gap it sits in, and
// both edges of that gap are judgements about operating, not measurements:
//
//   * UNDER it, one output block. Hl2RxDsp refreshes the reading once per
//     processed block — 1024 input samples, 21.3 ms at the HL2's slowest
//     48 kHz rate and less above it — so a threshold near that would call a
//     healthy receive path stale on ordinary scheduling jitter.
//   * OVER it, the shortest transmission. The briefest thing an operator can
//     deliberately do to a PTT is a tap, and a tap is a couple of hundred
//     milliseconds; CW break-in raises MOX on the first element and holds it
//     through the inter-element hang (Hl2Backend::setCwKeying), so even a
//     single character is ONE keyed span rather than one element.
//
// 150 ms sits in that gap: about seven blocks of margin under it, and under a
// tap over it, so every transmission blanks the pairing row rather than
// inverting it. Nobody has measured either edge on hardware and anything from
// roughly 100 to 200 ms would behave the same. The number is not the point;
// going quiet instead of asserting the opposite is.
inline constexpr std::int64_t kSliceStaleMs = 150;

// Is this WDSP meter value a measurement, or a sentinel?
inline bool adcMeterReadingIsReal(double dbfs) noexcept
{
    return std::isfinite(dbfs) && dbfs > kAdcMeterSilentDbfs;
}

// How far the post-DDC slice peak sits below wire full scale, in dB.
//
// Positive is headroom. The only figure in the pairing that is scale-free in
// the sense the crest factor is — it is still measured against WIRE full scale
// and so still says nothing about volts at the antenna.
inline double sliceHeadroomDb(double slicePeakDbfs) noexcept
{
    return -slicePeakDbfs;
}

enum class AdcPairing {
    // The pairing cannot be made: one side has not reported, or the slice side
    // is too old to stand against a live flag (kSliceStaleMs). Not a level and
    // not a verdict — a missing reading, and it must render as "not reported"
    // rather than as any number or any sentence. A side that last reported
    // four seconds ago against one that reported twenty milliseconds ago is
    // the same epistemic situation as a side that never reported at all.
    Unknown,
    // Neither side is near its limit. The uninteresting, and normal, case —
    // but NOT "the converter is comfortable". The pre-DDC bit is a saturated
    // clip counter, so a signal that clips occasionally leaves it clear; this
    // says only that neither side is complaining, which is less than saying
    // nothing clipped.
    BothClear,
    // THE DIAGNOSTIC CASE. The converter is overloading and this slice is not
    // where the energy is: the signal doing it is somewhere else in 0-38.4 MHz.
    // Backing off this slice's audio changes nothing; front-end attenuation or
    // a band filter does.
    ConverterOnly,
    // The slice is near wire full scale and the converter is not complaining.
    // Not a front-end problem — the level is arriving through the DDC's own
    // processing gain, and the lever is downstream.
    SliceOnly,
    // Both are hot: the strong signal IS in this slice, and the two readings
    // agree for once. The one case where either number alone would have done.
    BothHot,
};

// `haveHardwareFlag` is false until EP6 RADDR 0x00 has been seen at all, which
// is a different state from "seen, and clear" — Hl2Telemetry keeps them apart
// with std::optional for exactly this reason and so does this.
//
// `sliceReadingIsCurrent` is the caller's answer to "is the post-DDC side still
// moving?" — Hl2Backend passes `ago && *ago <= kSliceStaleMs`. It is an INPUT
// rather than a mute-flag special case on purpose: the frozen-against-live
// split appears whenever the DSP thread stops producing output blocks while
// EP6 responses keep arriving, and transmit is only the common way in.
inline AdcPairing adcPairing(bool haveSlicePeak,
                             double slicePeakDbfs,
                             bool sliceReadingIsCurrent,
                             bool haveHardwareFlag,
                             bool hardwareOverload) noexcept
{
    if (!haveHardwareFlag || !haveSlicePeak || !adcMeterReadingIsReal(slicePeakDbfs)) {
        return AdcPairing::Unknown;
    }
    // A stale slice peak cannot be paired with a live flag. See kSliceStaleMs:
    // the verdict is a sentence about a RELATIONSHIP between two readings, and
    // there is no relationship between a number from four seconds ago and a
    // flag from twenty milliseconds ago.
    if (!sliceReadingIsCurrent) {
        return AdcPairing::Unknown;
    }
    const bool sliceHot = sliceHeadroomDb(slicePeakDbfs) <= kSliceHotHeadroomDb;
    if (hardwareOverload) {
        return sliceHot ? AdcPairing::BothHot : AdcPairing::ConverterOnly;
    }
    return sliceHot ? AdcPairing::SliceOnly : AdcPairing::BothClear;
}

}  // namespace AetherSDR::hl2
