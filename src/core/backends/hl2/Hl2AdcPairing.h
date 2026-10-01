#pragma once

// The HL2's two ADC level readings, paired, as a pure decision (HERMES.md §13
// item 16). They are measured at different points and disagree by design:
//
//   * PRE-DDC — the gateware clip indicator (EP6 RADDR 0x00, DATA bit 24,
//     decoded in Hl2Telemetry::apply). It sees the whole 0-38.4 MHz input, so
//     a station far outside the slice can set it. It is an AND-reduction over
//     a 2-bit clip counter (control.v at 883a338), so it asserts only once the
//     counter saturates (~1.2 us of continuous clipping) and clears on the next
//     EP6 response (~1.3 ms at 48 kHz, one receiver). A clear bit is NOT "the
//     converter is comfortable", and the bit is not the count (HERMES.md
//     §11.4, §12.5; the count is §13 row 14).
//   * POST-DDC — WDSP's RXA_ADC_PK (adcmeter first in xrxa, ahead of nbp0). It
//     sees one slice, after the DDC and WDSP's half-band decimation.
//
// Read together they say where the energy is; apart, neither can. That is why
// the clip flag alone is the wrong driver for a gain decision.
//
// Nothing here is calibrated and the two sides are not on a common scale (dB
// re wire full scale vs. an unquantified DDC processing gain;
// Hl2DbReference::isCalibrated() is false). Labels claim only the pairing:
// "the converter is overloading while this slice sits 40 dB below full scale".
//
// DISPLAY ONLY, per IRadioBackend.h's health contract: nothing reads this
// verdict back to drive gain, AGC, drive or filters. It is a seam so the
// branches, which otherwise need a real converter in real overload, are
// testable; the liveness gates (kSliceStaleMs, sliceSideSampling) are the
// cases that most need it.

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
// The two sides do not stop together. During TX Hl2RxDsp holds the slice peak
// at its last receive value (the chain is clocked with silence), while
// Hl2Telemetry::adcOverload keeps updating (EP6 rides the IQ datagrams). Without
// a freshness test, an overload during transmit would read as "the signal is
// elsewhere" against a frozen quiet slice — the opposite of the truth. Past
// this age the verdict goes quiet instead.
//
// 150 ms is chosen, not measured: above one output block (21.3 ms at 48 kHz,
// so scheduling jitter isn't "stale") and below the shortest deliberate
// transmission (a PTT tap; CW break-in holds MOX across a whole character).
// Anything from ~100 to 200 ms behaves the same.
//
// This covers the tail of a transmission, not the head: at key-down the reading
// is still young and must age past the threshold (129-150 ms, plus a block or
// two because the mute reaches the DSP thread over a queued connection). The
// head is closed by the synchronous `sliceSideSampling` input below. This gate
// still catches every other way the DSP stops while EP6 continues: a stalled IQ
// stream, a chain between rebuilds, a starved DSP thread.
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
// are about to stop: Hl2Backend::applyRxAudioMute() passes `!muted` for the
// mute it is about to queue. So the input reads "not sampling" from the key
// edge through the whole unkey hold (#5497), and with the TX audio monitor on
// the chain is never muted, keeps sampling through the transmission, and the
// pairing keeps pairing. Deriving it from the mute itself rather than from
// the key state is load-bearing: the two differ for the length of the hold.
//
// This input is what covers the HEAD of a transmission, which the age alone
// cannot: see kSliceStaleMs. applyRxAudioMute() sets this synchronously while
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
