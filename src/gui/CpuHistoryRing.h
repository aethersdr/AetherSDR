#pragma once

// The Overview tab's CPU history (#2554): a bounded ring of process-level CPU
// readings and the slicing that turns it into chart points for a chosen
// timeframe — the CPU counterpart of MemoryHistoryRing, and deliberately shaped
// like it: gui-side, header-only, free of core includes, storing its own flat
// record of what SystemInfoDialog receives in a CpuSample. The bucket and gap
// rules are MemoryHistoryRing's, reused rather than repeated, so the two charts
// on one tab cannot disagree about where a bucket point sits.
//
// What one record carries beyond the process total: every thread that used a
// non-zero share of a core on that tick (name, tid, per-core percent). That is
// exact — a thread at 0 % contributes nothing to any chart — and compact, since
// most of the process's ~50 threads are idle on most ticks. The "top
// threads" chart picks its members from the whole window (the N threads
// with the highest MEAN share over it), then emits one point per bucket for
// each member, zero where the member did not appear, so the series line up
// bucket for bucket. Ranking by mean rather than by the newest tick keeps the
// membership from flickering between two threads trading fourth and fifth
// place every 1.5 s.
//
// The UI tick-lag fields ride the same record because they are sampled at the
// same instant (the dialog reads the meter when the CPU sample arrives); a
// tick rate of zero means "not measured on this tick" and is not plotted.

#include "MemoryHistoryRing.h"

#include <QHash>
#include <QPointF>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <deque>

namespace AetherSDR {

class CpuHistoryRing {
public:
    static constexpr int kSampleIntervalMs = MemoryHistoryRing::kSampleIntervalMs;
    static constexpr int kCapacity = MemoryHistoryRing::kCapacity;   // one hour at 1.5 s

    // How many threads the top-threads chart shows: "top 5 threads" (issue
    // body, Overview charts). The body asks for a stacked area; the chart
    // draws them as lines — see the PR body's design note.
    static constexpr int kTopThreads = 5;

    struct ThreadShare {
        quint64 tid{0};
        QString name;
        double  percentOfCore{0.0};   // > 0 by construction; zero shares are not stored
    };

    struct Record {
        qint64  wallMs{0};
        bool    valid{false};                 // false = no interval yet, or enumeration failed
        int     coreCount{0};
        double  processPercentOfCapacity{0.0};   // 0..100 of the whole machine
        quint64 busiestTid{0};
        QString busiestName;
        double  busiestPercentOfCore{0.0};       // 0..100 of one core
        QVector<ThreadShare> threads;            // non-zero shares only
        // UI event-loop tick lag over the interval this record covers; see
        // UiTickLagMeter. tickCount == 0 means the meter was not read.
        int     tickCount{0};
        double  tickLagMaxMs{0.0};
        double  tickLagMeanMs{0.0};
    };

    enum class Field { ProcessPercent, BusiestPercent, TickLagMax, TickLagMean };

    struct ThreadSeries {
        quint64 tid{0};
        QString name;
        double  meanPercentOfCore{0.0};   // over the window, absent ticks counting 0
        QVector<QPointF> points;          // one per bucket, aligned across members
    };

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

    static qint64 bucketMsFor(int rangeSeconds) { return MemoryHistoryRing::bucketMsFor(rangeSeconds); }
    static double connectGapSecondsFor(int rangeSeconds)
    {
        return MemoryHistoryRing::connectGapSecondsFor(rangeSeconds);
    }

    // Chart points for one scalar field over the last `rangeSeconds` ending at
    // `nowMs`; x in seconds since the cutoff. Same slicing as the memory ring:
    // raw up to five minutes, bucket averages at the bucket centre beyond.
    QVector<QPointF> series(Field field, int rangeSeconds, qint64 nowMs) const
    {
        QVector<QPointF> points;
        forEachBucket(rangeSeconds, nowMs,
                      [field](const Record& s) { return plottable(s, field); },
                      [&](double xSeconds, const QVector<const Record*>& bucket) {
                          double sum = 0.0;
                          for (const Record* s : bucket) {
                              sum += valueOf(*s, field);
                          }
                          points.push_back(QPointF(xSeconds, sum / bucket.size()));
                      });
        return points;
    }

    // The window's top `count` threads by mean share, each with one point per
    // bucket (zero where absent) so the chart's lines share one x axis. Ordered
    // highest mean first. Empty when no valid record is in the window.
    QVector<ThreadSeries> topThreadSeries(int count, int rangeSeconds, qint64 nowMs) const
    {
        QVector<ThreadSeries> result;
        if (count <= 0) {
            return result;
        }
        // Pass 1: mean share per tid over the valid records in the window.
        QHash<quint64, double> sumByTid;
        QHash<quint64, QString> nameByTid;
        int validRecords = 0;
        forEachBucket(rangeSeconds, nowMs,
                      [](const Record& s) { return s.valid; },
                      [&](double, const QVector<const Record*>& bucket) {
                          for (const Record* s : bucket) {
                              ++validRecords;
                              for (const ThreadShare& t : s->threads) {
                                  sumByTid[t.tid] += t.percentOfCore;
                                  nameByTid.insert(t.tid, t.name);
                              }
                          }
                      });
        if (validRecords == 0) {
            return result;
        }
        QVector<ThreadSeries> ranked;
        ranked.reserve(sumByTid.size());
        for (auto it = sumByTid.cbegin(); it != sumByTid.cend(); ++it) {
            ThreadSeries ts;
            ts.tid = it.key();
            ts.name = nameByTid.value(it.key());
            ts.meanPercentOfCore = it.value() / validRecords;
            ranked.push_back(ts);
        }
        // Ties by tid so the order is stable from one refresh to the next.
        std::sort(ranked.begin(), ranked.end(), [](const ThreadSeries& a, const ThreadSeries& b) {
            if (a.meanPercentOfCore != b.meanPercentOfCore) {
                return a.meanPercentOfCore > b.meanPercentOfCore;
            }
            return a.tid < b.tid;
        });
        if (ranked.size() > count) {
            ranked.resize(count);
        }
        // Pass 2: one point per bucket per member, aligned.
        forEachBucket(rangeSeconds, nowMs,
                      [](const Record& s) { return s.valid; },
                      [&](double xSeconds, const QVector<const Record*>& bucket) {
                          for (ThreadSeries& ts : ranked) {
                              double sum = 0.0;
                              for (const Record* s : bucket) {
                                  for (const ThreadShare& t : s->threads) {
                                      if (t.tid == ts.tid) {
                                          sum += t.percentOfCore;
                                          break;
                                      }
                                  }
                              }
                              ts.points.push_back(QPointF(xSeconds, sum / bucket.size()));
                          }
                      });
        return ranked;
    }

    static bool plottable(const Record& s, Field field)
    {
        if (!s.valid) {
            return false;
        }
        // Tick lag is measured only when the meter was read on this tick.
        if (field == Field::TickLagMax || field == Field::TickLagMean) {
            return s.tickCount > 0;
        }
        return true;
    }

    static double valueOf(const Record& s, Field field)
    {
        switch (field) {
        case Field::ProcessPercent: return s.processPercentOfCapacity;
        case Field::BusiestPercent: return s.busiestPercentOfCore;
        case Field::TickLagMax:     return s.tickLagMaxMs;
        case Field::TickLagMean:    return s.tickLagMeanMs;
        }
        return 0.0;
    }

private:
    // Walks the window's records in time order, grouping them by bucket (a
    // group of one at raw resolution), and hands each group to `visit` with
    // its x position in seconds since the cutoff. Records `keep` rejects are
    // skipped. The slicing rule is MemoryHistoryRing::series()'s.
    template <typename Keep, typename Visit>
    void forEachBucket(int rangeSeconds, qint64 nowMs, Keep keep, Visit visit) const
    {
        if (m_samples.empty() || rangeSeconds <= 0) {
            return;
        }
        const qint64 bucketMs = bucketMsFor(rangeSeconds);
        const qint64 endMs = bucketMs <= 1000 ? nowMs : (nowMs / bucketMs) * bucketMs;
        const qint64 cutoffMs = endMs - static_cast<qint64>(rangeSeconds) * 1000;

        if (bucketMs <= 1000) {
            for (const Record& s : m_samples) {
                if (s.wallMs < cutoffMs || s.wallMs > endMs || !keep(s)) {
                    continue;
                }
                visit(static_cast<double>(s.wallMs - cutoffMs) / 1000.0, QVector<const Record*>{&s});
            }
            return;
        }

        qint64 bucketStart = -1;
        QVector<const Record*> bucket;
        auto flush = [&]() {
            if (!bucket.isEmpty()) {
                visit(static_cast<double>(bucketStart + bucketMs / 2 - cutoffMs) / 1000.0, bucket);
            }
            bucket.clear();
        };
        for (const Record& s : m_samples) {
            if (s.wallMs < cutoffMs || s.wallMs > endMs || !keep(s)) {
                continue;
            }
            const qint64 start = (s.wallMs / bucketMs) * bucketMs;
            if (start != bucketStart) {
                flush();
                bucketStart = start;
            }
            bucket.push_back(&s);
        }
        flush();
    }

    std::deque<Record> m_samples;
};

}  // namespace AetherSDR
