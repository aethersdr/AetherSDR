#pragma once
#include <stdint.h>

// C-only data at the vendor boundary; latest completed decoder block, not the
// delayed output ring's exact audio timestamp. Caller owns channel lifetime.
// Pilot magnitude/thresholds are exact normalized-discriminator values before
// pilot AGC. Durations count DSP samples, milliseconds saturate at INT32_MAX.
// Stereo means the hysteretic INDY result, independently of forced-mono output.
typedef struct AetherWdspWbfmObservation {
    uint32_t observationSequence;
    int valid;
    int pilotLocked;
    uint32_t lockDurationMs, lockLossCount, reacquisitionCount;
    uint32_t observationDurationMs, stableDurationMs;
    uint32_t consecutiveHighBlocks, consecutiveLowBlocks, engageBlocks, releaseBlocks;
    double pilotMagnitude, pilotReleaseThreshold, pilotEngageThreshold;
} AetherWdspWbfmObservation;
