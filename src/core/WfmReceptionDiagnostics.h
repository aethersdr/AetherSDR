#pragma once

#include <cstdint>

namespace AetherSDR {
// Last completed WDSP decoder block, carried under the receiver PCM lifetime.
// The pilot indicator is a hysteretic coherent-magnitude detector, NOT a PLL.
// Magnitude is in normalized FM-discriminator units (75 kHz peak deviation =
// unity), measured after the 19 kHz filter and before pilot AGC. Its exact
// double value is neither RF strength, SNR nor a quality score.
// Durations use processed DSP samples, not wall-clock/audio-device time.
// Counts exclude the first acquisition, saturate at INT32_MAX, and reset on
// decoder construction/flush. Stable duration is capped at a five-second
// window and resets on either pilot-indicator transition. No pilot is normal
// mono fallback, not evidence of poor reception. Invalid snapshots carry no
// current reception claim. The backend expires them after 500 ms without data.
struct WfmReceptionDiagnostics {
    bool valid = false;
    double pilotMagnitude = 0.0;
    bool pilotLocked = false;
    std::uint32_t lockDurationMs = 0;
    std::uint32_t lockLossCount = 0;
    std::uint32_t reacquisitionCount = 0;
    std::uint32_t observationDurationMs = 0;
    std::uint32_t stableDurationMs = 0;
    double pilotEngageThreshold = 0.0;
    double pilotReleaseThreshold = 0.0;
    std::uint32_t consecutiveHighBlocks = 0;
    std::uint32_t consecutiveLowBlocks = 0;
    std::uint32_t engageBlocks = 0;
    std::uint32_t releaseBlocks = 0;
    std::uint32_t observationSequence = 0; // changes only after a completed DSP block
    bool operator==(const WfmReceptionDiagnostics&) const = default;
};
} // namespace AetherSDR
