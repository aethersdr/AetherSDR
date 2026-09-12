#pragma once

// The HL2's two ADC level readings, paired — as a pure decision.
//
// HERMES.md §13 item 16, A3 §7: "the single most useful diagnostic pairing on
// the HL2". There are two answers to "how hard is the converter being driven",
// they are measured at different points, and THEY DISAGREE BY DESIGN:
//
//   * PRE-DDC — the AD9866's own overload flag (EP6 RADDR 0x00, DATA bit 24,
//     decoded in MetisProtocol's Hl2Telemetry::apply). It is a level
//     comparator on the converter's input, so it sees the WHOLE 0-38.4 MHz
//     the converter sees. A broadcast station 20 MHz away sets it without
//     ever appearing in the slice the operator is listening to.
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
// every branch below is otherwise reachable only by driving a real converter
// into a real overload, which is not a thing a test suite can arrange.

#include <cmath>

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
    // One side has not reported. Not a level — a missing reading, and it must
    // render as "not reported" rather than as any number.
    Unknown,
    // Neither side is near its limit. The uninteresting, and normal, case.
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
inline AdcPairing adcPairing(bool haveSlicePeak,
                             double slicePeakDbfs,
                             bool haveHardwareFlag,
                             bool hardwareOverload) noexcept
{
    if (!haveHardwareFlag || !haveSlicePeak || !adcMeterReadingIsReal(slicePeakDbfs)) {
        return AdcPairing::Unknown;
    }
    const bool sliceHot = sliceHeadroomDb(slicePeakDbfs) <= kSliceHotHeadroomDb;
    if (hardwareOverload) {
        return sliceHot ? AdcPairing::BothHot : AdcPairing::ConverterOnly;
    }
    return sliceHot ? AdcPairing::SliceOnly : AdcPairing::BothClear;
}

}  // namespace AetherSDR::hl2
