#pragma once

// WWVB 60 kHz legacy AM/PWM time-code decoder. Format facts per the NIST
// WWVB time-code description (NIST SP 250-67): one bit per second, carrier
// power reduced 17 dB at each second start for 0.2 s (binary 0), 0.5 s
// (binary 1), or 0.8 s (marker); markers at seconds 0, 9, 19, 29, 39, 49,
// 59 — two consecutive markers (s59 -> s0) mark the minute boundary.
//
// Input contract: 24 kHz mono float32 from a slice tuned USB at 0.059 MHz —
// the 60 kHz carrier appears as a ~1000 Hz audio tone whose amplitude
// carries the PWM.
//
// Chain: FFT-refined tone search 800-1200 Hz once at acquisition -> complex
// mix at f0 -> LPF -> 100 Hz envelope series -> seconds edge = the AM drop
// (NEVER a phase edge: WWVB's 2012 BPSK layer flips phase +100 ms after the
// second and amplitude receivers are immune by design — INV-8) -> per-second
// PWM matched-filter classify (0.2/0.5/0.8 s low-power durations; margin =
// confidence; threshold adapts between the 10th/90th envelope percentiles)
// -> double-marker minute sync -> NIST WWVB BCD field map (MSB-first
// weights) -> TimeFrameVoter (shared with WwvDecoder).
//
// Pure DSP — no Qt (EB1/EB2). Streaming: process() accumulates internally,
// no whole-file transforms.

#include "TimeFrameVoter.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace AetherSDR {

class WwvbDecoder {
public:
    explicit WwvbDecoder(int sampleRateHz = 24000);
    ~WwvbDecoder();

    WwvbDecoder(const WwvbDecoder&) = delete;
    WwvbDecoder& operator=(const WwvbDecoder&) = delete;

    // Feed mono float32 samples; fires callbacks inline (same thread).
    void process(const float* mono, std::size_t n);

    void reset();

    // Arm the shared voter's absolute-plausibility gate (WS-4.5): a voted
    // timestamp farther than boundMinutes from the reference clock refuses to
    // lock. The engine plumbs the host clock here; default is disarmed so pure
    // decoder use (tests, corpus runners) is reference-free.
    void setPlausibility(std::function<TimeFields()> referenceNow,
                         int boundMinutes);

    ClockLockState state() const;
    ClockStation station() const;      // always Wwvb once acquiring
    std::int64_t samplesConsumed() const;

    // WS-7 acquisition telemetry: read-only snapshot assembled ON CALL from
    // state the decoder already keeps (tone search, envelope percentiles,
    // anchor, voter) — zero cost on the sample path, no feedback into decoding.
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
