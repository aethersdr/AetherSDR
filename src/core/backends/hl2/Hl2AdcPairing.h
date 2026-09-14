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
//     a level is crossed. `clip_cnt` is two bits wide and increments once per
//     control-clock tick while the synchronised sticky `rxclip` level is high,
//     so continuous clipping saturates it in roughly 1.2 us. The counter is
//     cleared by the next EP6 response (about 1.3 ms at 48 kHz with one
//     receiver); without a running stream it is an uncleared latch. Those
//     timings and the reset path are derived in HERMES.md section 11.4 from
//     gateware 883a338. Two things follow that the labels must respect: an
//     isolated clipping window can leave the bit clear, so a clear bit is NOT
//     "the converter is comfortable"; and the bit is not the counter.
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
// exceptions are the two liveness gates — kSliceStaleMs and sliceSideSampling
// — which need no overload at all, and both of them were WRONG before they
// existed: the first inverted the verdict for the whole of a transmission, the
// second for its leading 129-150 ms. The seam has now paid for itself twice, and
// each time the case that caught it was a case this file could express.

#include <chrono>
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
// tap over it. Nobody has measured either edge on hardware and anything from
// roughly 100 to 200 ms would behave the same. The number is not the point;
// going quiet instead of asserting the opposite is.
//
// THIS GATE COVERS THE TAIL OF A TRANSMISSION AND NOT THE HEAD, and an earlier
// version of this comment claimed it covered the whole of one. At key-down
// `ago` is the age of the last RECEIVE block — under one block period, 21.3 ms
// at 48 kHz — and it must still CLIMB to this threshold before the gate shuts.
// For that climb the held pre-transmit peak is fresh by age while the flag is
// live, which is the inverted sentence this gate exists to prevent, arriving
// for 129-150 ms of every key-down depending on where key-down falls inside a
// block (and longer still, because setKeying delivers the mute to the DSP
// thread over a QUEUED connection, so a block or two more may be sampled and
// re-stamped after the key goes down). K5PTB measured it against a real
// Hl2RxDsp in wall-clock: ConverterOnly to t=137 ms, Unknown from t=139 ms.
// RadioHealthDialog polls every 500 ms (kRefreshIntervalMs), so roughly three
// key-downs in ten land a refresh inside that window.
//
// The head is therefore closed by a SECOND, synchronous input rather than by
// this threshold — `sliceSideSampling` below, which the backend knows the
// instant it queues the mute. This gate stays because it catches every OTHER
// way the DSP thread can stop producing while EP6 keeps arriving: a stalled IQ
// stream, a chain between rebuilds, a starved DSP thread. Transmit was only
// the common way in, and it is now the one way in that is known in advance.
inline constexpr std::int64_t kSliceStaleMs = 150;

// The monotonic clock every timestamp in this family is taken from. One
// definition, because the gate below compares a stamp taken here against a
// stamp taken by Hl2RxDsp on the DSP thread, and two clocks that merely happen
// to agree today are not a comparison.
[[nodiscard]] inline std::int64_t steadyNowNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Turns "sampling was ASKED to resume" into "sampling HAS resumed".
//
// The synchronous input below (`sliceSideSampling`) is the backend's own
// knowledge of the mute it queues, and at key-DOWN that is exactly right:
// m_keyed is set before Hl2RxDsp stops sampling, so the gate shuts at or
// before sampling does. Early is the safe direction for an input whose whole
// job is to withhold an assertion.
//
// KEY-UP IS NOT SYMMETRIC, and the asymmetry is a defect this class closes.
// setKeying(false) clears m_keyed synchronously and setTxAudioMonitor(true)
// sets m_txMonitor synchronously, while BOTH deliver setAudioMuted(false) to
// the DSP thread over a QUEUED connection. The predicted answer therefore turns
// true before Hl2RxDsp has unmuted or produced a single new peak. If the held
// peak is still inside kSliceStaleMs — a short key-down, or the monitor
// switched on mid-transmission — the age gate is open too, and the verdict is
// asserted from a value that nothing is sampling: the same failure class the
// synchronous input was added to prevent, arriving through the other door.
//
// The proof that sampling HAS resumed is the stamp on the reading itself.
// Hl2RxDsp stores m_adcPeakAtNs only on the `!m_audioMuted` path, so a peak
// stamped after the resume was requested is necessarily a post-unmute sample.
// Nothing here predicts anything; it compares two timestamps.
//
// It costs a block or two of Unknown at key-up — one WDSP output block, ~21 ms
// at 48 kHz — before the first post-unmute peak lands. That is the safe
// direction again: an omission where there was an assertion.
class SliceSamplingGate {
public:
    // `requested` is the caller's `!(keyed && !txMonitor)`, passed from the
    // same site that queues setAudioMuted, so the two can never disagree.
    //
    // ONLY THE false->true EDGE MOVES THE BAR. Re-asserting a state that
    // already holds must not push it forward, or a setTxAudioMonitor(true)
    // repeated while already unmuted would keep invalidating live samples.
    void setRequested(bool requested, std::int64_t nowNs) noexcept
    {
        if (requested && !m_requested) {
            m_resumedAtNs = nowNs;
        }
        m_requested = requested;
    }

    // `peakAtNs` is Hl2RxDsp::adcPeakObservedAtNs(); 0 means never sampled,
    // which is "not reported" and not "not sampling" — but neither is a
    // reading to pair, and adcPairing() returns Unknown for both.
    [[nodiscard]] bool applied(std::int64_t peakAtNs) const noexcept
    {
        return m_requested && peakAtNs != 0 && peakAtNs > m_resumedAtNs;
    }

private:
    // Sampling is requested from construction. A backend that has never keyed
    // must not wait for an edge that never comes, and the zero bar then admits
    // any real reading — which is what "never interrupted" means.
    bool m_requested = true;
    std::int64_t m_resumedAtNs = 0;
};

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
    // is too old to stand against a live flag (kSliceStaleMs), or it is no
    // longer being sampled at all and its freshness is about to become a lie
    // (sliceSideSampling). Not a level and not a verdict — a missing reading,
    // and it must render as "not reported" rather than as any number or any
    // sentence. A side that last reported four seconds ago against one that
    // reported twenty milliseconds ago is the same epistemic situation as a
    // side that never reported at all — and so is one that reported twenty
    // milliseconds ago and will not report again.
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
// THE SLICE SIDE HAS TWO WAYS OF NOT BEING LIVE, and they are separate inputs
// because they are known at different times. Both must hold for the pairing to
// be a sentence about now.
//
// `sliceReadingIsCurrent` — OBSERVED, AFTER THE FACT. The caller's answer to
// "has the post-DDC side reported recently?"; Hl2Backend passes
// `ago && *ago <= kSliceStaleMs`. It is an input rather than a mute-flag
// special case on purpose: the frozen-against-live split appears whenever the
// DSP thread stops producing output blocks while EP6 responses keep arriving,
// and there is no flag for a stalled IQ stream or a starved thread. Its cost
// is that it can only notice the freeze once the age has had time to grow.
//
// `sliceSideSampling` — KNOWN IN ADVANCE, for the one case where that is
// possible. Hl2RxDsp does not sample RXA_ADC_PK while muted, and Hl2Backend is
// the code that queues that mute, so it knows synchronously that the readings
// are about to stop: it passes `!(m_keyed && !m_txMonitor)`, mirroring
// `muteWhileKeyed` in setKeying. That mirroring is load-bearing rather than
// tidy — with the TX audio monitor on, the chain keeps sampling through the
// transmission, the reading keeps moving, and the pairing must keep pairing.
//
// This input is what covers the HEAD of a transmission, which the age alone
// cannot: see kSliceStaleMs. `m_keyed` is set synchronously in setKeying while
// the mute rides a queued connection to the DSP thread, so the gate shuts at
// or before the instant sampling actually stops — early is the safe direction
// here, because the failure it prevents is an assertion, not an omission.
inline AdcPairing adcPairing(bool haveSlicePeak,
                             double slicePeakDbfs,
                             bool sliceReadingIsCurrent,
                             bool sliceSideSampling,
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
    // ...and a peak that is still FRESH but whose source has just stopped is
    // the same thing arriving a fraction of a second earlier. At key-down the
    // held reading has an honest age of a few milliseconds and describes a band
    // the operator is no longer listening to.
    if (!sliceSideSampling) {
        return AdcPairing::Unknown;
    }
    const bool sliceHot = sliceHeadroomDb(slicePeakDbfs) <= kSliceHotHeadroomDb;
    if (hardwareOverload) {
        return sliceHot ? AdcPairing::BothHot : AdcPairing::ConverterOnly;
    }
    return sliceHot ? AdcPairing::SliceOnly : AdcPairing::BothClear;
}

}  // namespace AetherSDR::hl2
