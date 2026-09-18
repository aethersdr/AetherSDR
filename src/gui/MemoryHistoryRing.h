#pragma once

// The Memory tab's history (#2554): a bounded ring of process-memory readings
// and the slicing that turns it into chart points for a chosen timeframe.
//
// Deliberately gui-side, header-only, and free of core includes. The record it
// stores is its own flat copy of what the dialog receives in a MemorySample —
// SystemInfoDialog::applyMemorySample() does the conversion — so this header adds
// no gui→core touchpoint for the aetherd burndown (docs/architecture/
// aetherd-touchpoints.md counts includers per engine header, and this class is
// read by the dialog alone). The compacting seven-day history the issue
// describes (SystemInfoHistory, the NetworkDiagnosticsHistory pattern) is the
// Overview increment's; this holds the selector's longest window — one hour at
// the collector's 1.5 s cadence — raw, and nothing more.
//
// Slicing follows NetworkDiagnosticsDialog::updateCharts() so the two dialogs'
// charts read alike: raw points at one-second resolution up to five minutes,
// bucket averages of max(5 s, range / 300) beyond placed at the bucket centre,
// x = seconds since the window's cutoff, which is what
// TimeSeriesGraphWidget::setSeries() expects alongside its rangeSeconds.
// A record the platform could not fill is not plotted: an invalid sample, or
// a field left at zero (Windows never reports virtualBytes, Linux leaves
// privateBytes at zero without /proc/self/smaps_rollup — MemoryTelemetry.cpp).
// Zero is never a real reading for any of the four fields, so it means "unset".

#include <QPointF>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <deque>

namespace AetherSDR {

class MemoryHistoryRing {
public:
    // The collector's cadence (SystemInfoCollector::kSampleIntervalMs), repeated
    // here so the gap rule needs no core include. SystemInfoDialog.cpp — the one
    // translation unit that sees both headers — static_asserts they are equal.
    static constexpr int kSampleIntervalMs = 1500;

    // One hour of 1.5 s samples: the selector's longest timeframe.
    static constexpr int kCapacity = 2400;

    // One reading, as the ring stores it: the fields the readouts and the chart
    // need, nothing that ties the ring to how they were captured.
    struct Record {
        qint64  wallMs{0};              // capture-time wall clock, ms since epoch
        bool    valid{false};           // false = the platform reported nothing usable
        QString residentMetric;         // physicalFootprint / workingSet / vmRss / unsupported
        quint64 residentBytes{0};
        quint64 peakResidentBytes{0};
        quint64 privateBytes{0};
        quint64 virtualBytes{0};
    };

    // Which field of a Record a series plots.
    enum class Field { Resident, Peak, Private, Virtual };

    void push(const Record& record)
    {
        m_samples.push_back(record);
        while (static_cast<int>(m_samples.size()) > kCapacity) {
            m_samples.pop_front();
        }
    }

    void clear() { m_samples.clear(); }
    int  size() const { return static_cast<int>(m_samples.size()); }
    bool isEmpty() const { return m_samples.empty(); }
    const Record* latest() const { return m_samples.empty() ? nullptr : &m_samples.back(); }

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
            for (const Record& s : m_samples) {
                if (s.wallMs < cutoffMs || s.wallMs > endMs || !plottable(s, field)) {
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
                // Bucket centre, as the network dialog places its own.
                points.push_back(QPointF(
                    static_cast<double>(bucketStart + bucketMs / 2 - cutoffMs) / 1000.0,
                    sum / count));
            }
            sum = 0.0;
            count = 0;
        };
        for (const Record& s : m_samples) {
            if (s.wallMs < cutoffMs || s.wallMs > endMs || !plottable(s, field)) {
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
        const qint64 stepMs = std::max<qint64>(kSampleIntervalMs, bucketMsFor(rangeSeconds));
        return 3.0 * static_cast<double>(stepMs) / 1000.0;
    }

    static double megabytes(quint64 bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

    // A reading the platform actually produced: valid, and the field non-zero
    // (see the header comment — zero means the platform left it unset).
    static bool plottable(const Record& s, Field field) { return s.valid && valueOf(s, field) > 0; }

    static quint64 valueOf(const Record& s, Field field)
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
    std::deque<Record> m_samples;
};

}  // namespace AetherSDR
