#pragma once

// AetherClock acquisition telemetry (WS-7 / PRD-C): the engine's ~1 Hz
// read-only diagnostics snapshot that drives the pre-lock funnel, the verdict
// line and the debug pane. PARALLEL to — never extending — the frozen
// ClockAlignmentFrame contract (that field set is cross-PR frozen and stays
// untouched; diagnostics ride this separate struct). Every field is a real
// measured quantity or a real gate verdict mirrored from the decode chain:
// nothing is display-derived, and nothing here feeds back into decode
// behavior.

#include "TimeFrameVoter.h"

#include <QMetaType>

namespace AetherSDR {

struct ClockDiagnostics {
    // stage 1 — carrier
    float toneSnrDb = 0.0f;    // WWVB: tone-search peak/median dB; WWV: tick fold dB
    float pwmContrast = 0.0f;  // WWVB: p90/p10 envelope contrast (0 when n/a)
    bool  toneDetected = false;
    // stage 2 — timing
    bool  phaseLocked = false;
    float delayEstMs = 0.0f;   // NaN when the decoder has no delay estimate
    // stage 3 — frame
    bool  anchored = false;
    int   badFrameStreak = 0;
    // stage 4 — decode (engine-side ring: % of the last 60 s that classified)
    int   classifiedPct = 0;
    // stage 5 — vote
    int   framesInWindow = 0;
    int   windowSize = 0;
    float voteQuality = 0.0f;  // voter lockConfidence() raw 0..1
    quint8 refusalReason = 0;  // ClockLockRefusal
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ClockDiagnostics)
// Queued-signal registration for the raw per-frame decode the engine re-emits
// beside the diagnostics (frameDecoded): decoded fields that previously died
// at the engine boundary.
Q_DECLARE_METATYPE(AetherSDR::ClockFrameInfo)
