#pragma once

#include <QVector>
#include <QtGlobal>
#include <array>
#include <cmath>

namespace AetherSDR {

inline constexpr std::array<int, 9> kWaterfallMarkerIntervals{
    0, 15, 30, 60, 300, 600, 900, 1800, 3600};

inline int validWaterfallMarkerInterval(int seconds)
{
    for (const int value : kWaterfallMarkerIntervals) {
        if (value == seconds) {
            return seconds;
        }
    }
    return 0;
}

// Capture metadata follows the same physical ring slot as the signal pixels.
// Retaining the predecessor also preserves a crossing at the viewport bottom.
struct WaterfallTimeRow {
    qint64 timestampMs{0};
    qint64 previousMs{0};
};

struct WaterfallTimeMarker {
    qint64 timestampMs{0};
    qreal y{0};
};

inline QVector<WaterfallTimeMarker> waterfallTimeMarkers(
    const QVector<WaterfallTimeRow>& rows, int head, int seconds,
    qreal sampleOffsetRows, qreal displayHeight)
{
    QVector<WaterfallTimeMarker> markers;
    if (validWaterfallMarkerInterval(seconds) == 0 || rows.isEmpty()
        || head < 0 || head >= rows.size() || displayHeight <= 0
        || !std::isfinite(sampleOffsetRows)) {
        return markers;
    }
    const qint64 intervalMs = qint64(seconds) * 1000;
    for (int age = 0; age < rows.size(); ++age) {
        const WaterfallTimeRow& row = rows[(head + age) % rows.size()];
        // No invented rows across a gap, and no repeated boundary on a batch
        // of equal timestamps. A backward clock adjustment starts a new run.
        if (row.previousMs <= 0 || row.timestampMs <= row.previousMs
            || row.timestampMs / intervalMs == row.previousMs / intervalMs) {
            continue;
        }
        const qreal y = (age - sampleOffsetRows) * displayHeight / rows.size();
        if (y >= 0 && y < displayHeight) {
            // Label the clock boundary, not the first packet's arrival time
            // after it. Keep the line attached to that captured signal row.
            const qint64 boundaryMs = (row.timestampMs / intervalMs) * intervalMs;
            markers.push_back({boundaryMs, y});
        }
    }
    return markers;
}

} // namespace AetherSDR
