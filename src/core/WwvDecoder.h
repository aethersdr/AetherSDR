#pragma once

// WWV/WWVH 100 Hz-subcarrier BCD time-code decoder (NIST SP 432), streaming
// port of the AetherClock reference chain (research/wwv_decode_proto.py).
// Input: 24 kHz mono float32 from a slice tuned USB at carrier - 1 kHz, so the
// carrier is a 1000 Hz tone, the BCD subcarrier 900/1100 Hz sidebands, and the
// seconds tick images at 2000 Hz (WWV) or 2200 Hz (WWVH), which tags the
// station. Marker-only anchoring is degenerate mod 10 s; the s0 subcarrier hole
// and minute-increment scoring disambiguate. Chain detail in the .cpp. Pure
// DSP, no Qt (EB1/EB2); process() streams.

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
