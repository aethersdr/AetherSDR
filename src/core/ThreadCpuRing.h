#pragma once

#include "SystemInfo.h"

#include <QHash>
#include <QVector>

namespace AetherSDR {

// Recent per-thread CPU readings for the Threads tab's Peak and sparkline
// columns (#2554): a fixed small count per thread, order only, no timestamps.
// Not SystemInfoHistory (the timeframe-compacting ring for charts).
// kSamples = 40 at the 1.5 s cadence = 60 s, so both columns cover the same
// minute. Header-only, Qt-Core-only so the peak/eviction maths is testable.
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
