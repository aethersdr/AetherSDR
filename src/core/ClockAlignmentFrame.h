#pragma once

// AetherClock alignment contract — one emission per classified second while
// the engine runs; drives the PR-B alignment display. The field set is
// FROZEN as a cross-PR contract (PRD-A §5): PR-B consumes exactly this.

#include <QMetaType>
#include <QVector>

namespace AetherSDR {

struct ClockAlignmentFrame {
    qint64  hostUtcMs = 0;       // host clock (UTC ms) at the detected second edge
    int     secondOfFrame = -1;  // 0-59 once frame-synced, else -1
    QVector<float> envelope;     // received envelope over the 1 s window
                                 // (series rate = envelope.size() per second)
    QVector<float> expected;     // matched template of the decoded symbol, same rate
    int     edgeOffsetMs = 0;    // signed drift of the detected edge vs the nominal window start, ms (~0 drift-free)
    int     symbol = -1;         // 0, 1, 2 = marker, -1 = unknown
    float   confidence = 0.0f;   // classification margin, 0..1
    quint8  station = 0;         // ClockStation enum value
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ClockAlignmentFrame)
