#pragma once

// WWV/WWVH 100 Hz-subcarrier BCD time-code decoder — streaming port of the
// gate-passed AetherClock reference chain (research/wwv_decode_proto.py).
// Format facts per the NIST WWV/WWVH time-code table (NIST SP 432).
//
// Input contract: 24 kHz mono float32 from a slice tuned USB at
// (carrier - 1 kHz). In that spectrum the RF carrier is a 1000 Hz audio tone,
// the 100 Hz BCD subcarrier appears as 900/1100 Hz sidebands, and the WWV
// 1000 Hz seconds tick images at 2000 Hz (WWVH's 1200 Hz tick at 2200 Hz —
// which tick band carries energy tags the station).
//
// Chain: analytic bandpass 700-1300 Hz -> envelope -> coherent 100 Hz demod
// (25 Hz LPF) -> 200 Hz amplitude series -> tick-phase sync (2000/2200 Hz
// band) -> per-second matched-filter classify (170/470/770 ms templates at
// +30 ms; margin = confidence) -> marker frame sync (P markers at seconds
// 9/19/29/39/49/59; marker-only anchoring is DEGENERATE mod 10 s —
// disambiguated via the s0 minute-mark subcarrier hole and minute-increment
// scoring) -> NIST BCD field map -> TimeFrameVoter.
//
// Pure DSP — no Qt (EB1/EB2). Streaming: process() accumulates internally,
// no whole-file transforms.

#include "TimeFrameVoter.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace AetherSDR {

class WwvDecoder {
public:
    explicit WwvDecoder(int sampleRateHz = 24000);
    ~WwvDecoder();

    WwvDecoder(const WwvDecoder&) = delete;
    WwvDecoder& operator=(const WwvDecoder&) = delete;

    // Feed mono float32 samples; fires callbacks inline (same thread) as
    // seconds/frames/time updates become available.
    void process(const float* mono, std::size_t n);

    void reset();

    // Arm the shared voter's absolute-plausibility gate (WS-4.5): a voted
    // timestamp farther than boundMinutes from the reference clock refuses to
    // lock. The engine plumbs the host clock here; default is disarmed so pure
    // decoder use (tests, corpus runners) is reference-free.
    void setPlausibility(std::function<TimeFields()> referenceNow,
                         int boundMinutes);

    ClockLockState state() const;
    ClockStation station() const;      // Wwv or Wwvh once tick-tagged
    std::int64_t samplesConsumed() const;

    // WS-7 acquisition telemetry: read-only snapshot assembled ON CALL from
    // state the decoder already keeps (tick fold, delay estimate, anchor,
    // voter) — zero cost on the sample path, no feedback into decoding.
    ClockDecoderDiagnostics diagnostics() const;

    // Callbacks (any may be left unset).
    std::function<void(const ClockSecondInfo&)> onSecond;
    std::function<void(const ClockFrameInfo&)> onFrame;
    std::function<void(const ClockTimeInfo&)> onTime;  // voted updates while locked
    std::function<void(ClockLockState)> onStateChanged;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR
