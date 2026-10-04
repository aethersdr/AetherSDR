#pragma once

// The HL2's two ADC level readings, paired, as a pure decision (HERMES.md §13
// item 16). PRE-DDC is the gateware clip bit (EP6 RADDR 0x00, DATA bit 24): it
// sees all 0-38.4 MHz, asserts only once a 2-bit clip counter saturates (~1.2 us
// of continuous clipping; control.v at 883a338) and clears on the next EP6
// response, so a clear bit is not "comfortable" (HERMES.md §11.4, §12.5).
// POST-DDC is WDSP's RXA_ADC_PK: one slice, after the DDC and half-band decimation.
// The two sides share no calibrated scale (Hl2DbReference::isCalibrated() is
// false). DISPLAY ONLY per IRadioBackend.h's health contract: nothing feeds this
// verdict back into gain, AGC, drive or filters.

#include <chrono>
#include <cmath>
#include <cstdint>

namespace AetherSDR::hl2 {

// Below this, a WDSP meter reading is a sentinel, not a measurement:
// WdspChannel::meter() returns -300.0 for a TX channel, WDSP's meter.c writes
// -400.0 for a stopped stage (and 10*log10(peak + 1e-40) for all-zero input).
inline constexpr double kAdcMeterSilentDbfs = -200.0;

// How close to wire full scale the post-DDC slice must sit to count as "hot".
// A display boundary only (the two sides share no scale); nothing acts on it.
inline constexpr double kSliceHotHeadroomDb = 3.0;

// Max age of the post-DDC reading that may be paired with a live overload flag.
// During TX Hl2RxDsp holds its last slice peak while EP6 overload keeps
// updating, so a stale peak would misreport "signal is elsewhere". 150 ms is
// chosen, not measured: above one 48 kHz output block (21.3 ms), below a PTT tap.
// This covers stalls and the TX tail; the TX head is `sliceSideSampling`.
inline constexpr std::int64_t kSliceStaleMs = 150;

// One clock for every timestamp in this family: the gate compares a stamp taken
// here against one Hl2RxDsp takes on the DSP thread.
[[nodiscard]] inline std::int64_t steadyNowNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Turns "sampling was ASKED to resume" into "sampling HAS resumed".
// At key-down the synchronous request shuts the gate at or before sampling stops.
// At key-up setKeying(false)/setTxAudioMonitor(true) flip the request
// synchronously while setAudioMuted(false) reaches the DSP thread queued, so the
// held peak could look fresh. Hl2RxDsp stamps m_adcPeakAtNs only while unmuted,
// so a peak stamped after the resume request is a post-unmute sample. Cost: one
// ~21 ms block of Unknown at key-up.
class SliceSamplingGate {
public:
    // `requested` is the caller's `!(keyed && !txMonitor)`, passed from the site
    // that queues setAudioMuted. Only the false->true edge moves the bar, so a
    // repeated setTxAudioMonitor(true) cannot invalidate live samples.
    void setRequested(bool requested, std::int64_t nowNs) noexcept
    {
        if (requested && !m_requested) {
            m_resumedAtNs = nowNs;
        }
        m_requested = requested;
    }

    // `peakAtNs` is Hl2RxDsp::adcPeakObservedAtNs(); 0 means never sampled.
    [[nodiscard]] bool applied(std::int64_t peakAtNs) const noexcept
    {
        return m_requested && peakAtNs != 0 && peakAtNs > m_resumedAtNs;
    }

private:
    // Requested from construction, so a never-keyed backend admits any real reading.
    bool m_requested = true;
    std::int64_t m_resumedAtNs = 0;
};

// Is this WDSP meter value a measurement, or a sentinel?
inline bool adcMeterReadingIsReal(double dbfs) noexcept
{
    return std::isfinite(dbfs) && dbfs > kAdcMeterSilentDbfs;
}

// Post-DDC slice peak below wire full scale, in dB; positive is headroom. Says
// nothing about volts at the antenna.
inline double sliceHeadroomDb(double slicePeakDbfs) noexcept
{
    return -slicePeakDbfs;
}

enum class AdcPairing {
    // The pairing cannot be made: a side has not reported, the slice reading is
    // stale (kSliceStaleMs), or slice sampling has stopped (sliceSideSampling).
    // Must render as "not reported", never as a number or sentence.
    Unknown,
    // Neither side is near its limit. Not "nothing clipped": the clip counter
    // leaves occasional clipping unflagged.
    BothClear,
    // The converter overloads but this slice is not where the energy is: the
    // cause is elsewhere in 0-38.4 MHz. Front-end attenuation or a band filter helps;
    // this slice's audio level does not.
    ConverterOnly,
    // Slice near wire full scale, converter clear: the level comes through DDC
    // processing gain, so the lever is downstream.
    SliceOnly,
    // Both hot: the strong signal is in this slice.
    BothHot,
};

// `haveHardwareFlag` is false until EP6 RADDR 0x00 has been seen at all
// (distinct from "seen, clear"). The slice side must be live on both inputs:
// `sliceReadingIsCurrent` (observed: Hl2Backend passes `ago && *ago <=
// kSliceStaleMs`; catches stalled IQ or a starved DSP thread, after the fact) and
// `sliceSideSampling` (known in advance: Hl2Backend::applyRxAudioMute() passes
// `!muted` for the mute it is about to queue, covering the TX head and unkey
// hold (#5497); with the TX monitor on, the chain stays unmuted and keeps pairing).
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
    // A stale slice peak has no relationship to a live flag.
    if (!sliceReadingIsCurrent) {
        return AdcPairing::Unknown;
    }
    // A fresh peak whose source just stopped (key-down) is equally unpairable.
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
