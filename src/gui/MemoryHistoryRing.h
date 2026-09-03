#pragma once

// The Memory tab's history (#2554): a bounded ring of MemorySample readings and
// the slicing that turns it into chart points for a chosen timeframe.
//
// Deliberately gui-side and header-only. The compacting seven-day history the
// issue describes (SystemInfoHistory, the NetworkDiagnosticsHistory pattern) is
// the Overview increment's; this holds the selector's longest window — one
// hour at the collector's 1.5 s cadence — raw, and nothing more. A new
// src/core header would be one more gui→core touchpoint for the aetherd
// burndown to carry, for a class that only the dialog reads.
//
// Slicing follows NetworkDiagnosticsDialog::updateCharts() so the two dialogs'
// charts read alike: raw points at one-second resolution up to five minutes,
// bucket averages of max(5 s, range / 300) beyond, x = seconds since the
// window's cutoff, which is what TimeSeriesGraphWidget::setSeries() expects
// alongside its rangeSeconds.

#include "core/SystemInfoCollector.h"

#include <QPointF>
#include <QVector>

#include <algorithm>
#include <deque>

namespace AetherSDR {

class MemoryHistoryRing {
public:
    // One hour of 1.5 s samples: the selector's longest timeframe.
    static constexpr int kCapacity = 2400;

    // Which field of MemorySample a series plots.
    enum class Field { Resident, Peak, Private, Virtual };

    void push(const MemorySample& sample)
    {
        m_samples.push_back(sample);
        while (static_cast<int>(m_samples.size()) > kCapacity) {
            m_samples.pop_front();
        }
    }

    void clear() { m_samples.clear(); }
    int  size() const { return static_cast<int>(m_samples.size()); }
    bool isEmpty() const { return m_samples.empty(); }
    const MemorySample* latest() const { return m_samples.empty() ? nullptr : &m_samples.back(); }

    // The network dialog's bucket rule, exposed so a test can pin it and so the
    // dialog and this class cannot disagree about it.
    static qint64 bucketMsFor(int rangeSeconds)
    {
        return rangeSeconds <= 5 * 60
            ? 1000
            : std::max<qint64>(5000, (static_cast<qint64>(rangeSeconds) * 1000) / 300);
    }

    // Chart points for `field` over the last `rangeSeconds` ending at `nowMs`,
    // in megabytes, x in seconds since the cutoff (0..rangeSeconds). Samples
    // older than the window are skipped; beyond five minutes samples sharing a
    // bucket are averaged so a one-hour window is ~300 points, not 2400.
    QVector<QPointF> series(Field field, int rangeSeconds, qint64 nowMs) const
    {
        QVector<QPointF> points;
        if (m_samples.empty() || rangeSeconds <= 0) {
            return points;
        }
        const qint64 bucketMs = bucketMsFor(rangeSeconds);
        const qint64 endMs = bucketMs <= 1000 ? nowMs : (nowMs / bucketMs) * bucketMs;
        const qint64 cutoffMs = endMs - static_cast<qint64>(rangeSeconds) * 1000;

        if (bucketMs <= 1000) {
            points.reserve(static_cast<int>(m_samples.size()));
            for (const MemorySample& s : m_samples) {
                if (s.wallMs < cutoffMs || s.wallMs > endMs) {
                    continue;
                }
                points.push_back(QPointF(static_cast<double>(s.wallMs - cutoffMs) / 1000.0,
                                         megabytes(valueOf(s, field))));
            }
            return points;
        }

        qint64 bucketStart = -1;
        double sum = 0.0;
        int    count = 0;
        auto flush = [&]() {
            if (count > 0) {
                points.push_back(QPointF(static_cast<double>(bucketStart - cutoffMs) / 1000.0,
                                         sum / count));
            }
            sum = 0.0;
            count = 0;
        };
        for (const MemorySample& s : m_samples) {
            if (s.wallMs < cutoffMs || s.wallMs > endMs) {
                continue;
            }
            const qint64 start = (s.wallMs / bucketMs) * bucketMs;
            if (start != bucketStart) {
                flush();
                bucketStart = start;
            }
            sum += megabytes(valueOf(s, field));
            ++count;
        }
        flush();
        return points;
    }

    // How far apart two chart points may be before the line breaks: three
    // samples' worth at raw resolution, three buckets' worth once the range
    // is bucketed. A fixed 4.5 s would isolate every 12 s bucket point of a
    // one-hour view and draw nothing — the smoke that found it (2026-09-02).
    static double connectGapSecondsFor(int rangeSeconds)
    {
        const qint64 stepMs = std::max<qint64>(SystemInfoCollector::kSampleIntervalMs,
                                               bucketMsFor(rangeSeconds));
        return 3.0 * static_cast<double>(stepMs) / 1000.0;
    }

    static double megabytes(quint64 bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

    static quint64 valueOf(const MemorySample& s, Field field)
    {
        switch (field) {
        case Field::Resident: return s.residentBytes;
        case Field::Peak:     return s.peakResidentBytes;
        case Field::Private:  return s.privateBytes;
        case Field::Virtual:  return s.virtualBytes;
        }
        return 0;
    }

private:
    std::deque<MemorySample> m_samples;
};

}  // namespace AetherSDR
