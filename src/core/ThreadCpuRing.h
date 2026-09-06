#pragma once

#include "SystemInfo.h"

#include <QHash>
#include <QVector>

namespace AetherSDR {

// Recent per-thread CPU readings, for the Threads tab's Peak and sparkline
// columns (#2554).
//
// This is NOT SystemInfoHistory. That is the compacting ring the issue specs
// for charts over a timeframe selector — an hour raw, seven days bucketed —
// and it stays deferred with the Overview tab, along with the charts that are
// its only consumer. This holds a fixed, small number of readings per thread
// and nothing else: no timestamps, no compaction, no timeframe. Two columns is
// the whole requirement, and a column does not need to know when a sample was
// taken, only what order they came in.
//
// The window resolves an inconsistency in the issue's own column list. It asks
// for "Peak (last 60 s)" and a "60-sample mini-chart" in adjacent rows, but at
// the collector's 1.5 s cadence those are different windows — 60 samples is
// 90 s. kSamples = 40 makes both columns mean the same minute.
//
// Pure, header-only and Qt-Core-only by design: the eviction and peak maths is
// where an off-by-one would silently mislabel a column, and here it is testable
// without constructing a widget.
class ThreadCpuRing {
public:
    // 40 × the collector's 1500 ms cadence = 60 s.
    static constexpr int kSamples = 40;

    // Append one sample per thread and retire everything absent from it.
    //
    // Retirement is not tidiness: threads come and go for the life of the
    // process, and a ring that only ever grew would hold a reading for every
    // thread that had ever existed, so "peak" would keep reporting the high
    // water mark of a thread that exited an hour ago.
    void update(const QVector<ThreadCpuSample>& samples)
    {
        QHash<quint64, QVector<double>> next;
        next.reserve(samples.size());
        for (const ThreadCpuSample& sample : samples) {
            QVector<double> series = m_byTid.value(sample.tid);
            series.push_back(sample.cpuPercentOfCore);
            while (series.size() > kSamples) {
                series.removeFirst();
            }
            next.insert(sample.tid, series);
        }
        m_byTid = std::move(next);
    }

    // Oldest first, newest last, at most kSamples. Empty for a thread seen for
    // the first time this interval or not at all — the sparkline draws nothing
    // rather than a flat line at zero, which would read as an idle thread.
    QVector<double> seriesFor(quint64 tid) const { return m_byTid.value(tid); }

    // Highest reading still inside the window. 0.0 when the thread is unknown,
    // which is the same thing the column shows for a thread that has genuinely
    // used no CPU — acceptable because a brand-new thread is at 0 % anyway by
    // the rule in cpuPercentBetween.
    double peakFor(quint64 tid) const
    {
        const auto found = m_byTid.constFind(tid);
        if (found == m_byTid.constEnd()) {
            return 0.0;
        }
        double peak = 0.0;
        for (const double value : *found) {
            if (value > peak) {
                peak = value;
            }
        }
        return peak;
    }

    // Dropped wholesale when sampling stops. A peak spanning a gap in which
    // nothing was sampled would be a claim about a minute that was never
    // observed — the dialog stops the collector when it is hidden, so that gap
    // is the normal case rather than an edge one.
    void clear() { m_byTid.clear(); }

    int trackedThreads() const { return static_cast<int>(m_byTid.size()); }

private:
    QHash<quint64, QVector<double>> m_byTid;
};

}  // namespace AetherSDR
