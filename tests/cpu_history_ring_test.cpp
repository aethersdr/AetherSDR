// #2554 (Overview tab) -- CpuHistoryRing: retention, scalar slicing on the bucket
// rule it shares with MemoryHistoryRing, and the window-level top-N selection
// that feeds the "top threads" chart. Pure logic; no widget, no socket,
// no radio. Every number below is CONSTRUCTED: the samples stand for no captured
// process and exercise arithmetic and routing only.
#include "gui/CpuHistoryRing.h"

#include <QCoreApplication>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    auto a_ = (actual); auto e_ = (expected); \
    if (a_ != e_) { \
        std::fprintf(stderr, "FAIL %s:%d  expected %s, got %s\n", __FILE__, __LINE__, \
                     QString("%1").arg(e_).toUtf8().constData(), \
                     QString("%1").arg(a_).toUtf8().constData()); \
        ++g_failures; \
    } \
} while (0)

#define EXPECT_TRUE(cond, what) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        ++g_failures; \
    } \
} while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

static CpuHistoryRing::Record at(qint64 wallMs, double processPercent,
                                 std::initializer_list<CpuHistoryRing::ThreadShare> threads = {})
{
    CpuHistoryRing::Record r;
    r.wallMs = wallMs;
    r.valid = true;
    r.coreCount = 8;
    r.processPercentOfCapacity = processPercent;
    for (const auto& t : threads) {
        r.threads.push_back(t);
        if (t.percentOfCore > r.busiestPercentOfCore) {
            r.busiestPercentOfCore = t.percentOfCore;
            r.busiestTid = t.tid;
            r.busiestName = t.name;
        }
    }
    return r;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using Field = CpuHistoryRing::Field;
    const qint64 kT0 = 1'000'000'000'000;   // an arbitrary epoch-ms origin

    // 1. Empty ring: no series, no members, latest() null.
    {
        CpuHistoryRing ring;
        EXPECT_TRUE(ring.isEmpty(), "new ring is empty");
        EXPECT_TRUE(ring.latest() == nullptr, "latest() is null on an empty ring");
        EXPECT_EQ(ring.series(Field::ProcessPercent, 60, kT0).size(), 0);
        EXPECT_EQ(ring.topThreadSeries(5, 60, kT0).size(), 0);
    }

    // 2. Retention: kCapacity records, oldest evicted first.
    {
        CpuHistoryRing ring;
        for (int i = 0; i < CpuHistoryRing::kCapacity + 5; ++i) {
            ring.push(at(kT0 + i * 1500, static_cast<double>(i)));
        }
        EXPECT_EQ(ring.size(), CpuHistoryRing::kCapacity);
        EXPECT_TRUE(near(ring.latest()->processPercentOfCapacity,
                         static_cast<double>(CpuHistoryRing::kCapacity + 4)),
                    "newest record survives eviction");
        // The evicted ones were the oldest: the 1-hour window ending at the
        // newest record holds exactly what fits, none older.
        const auto pts = ring.series(Field::ProcessPercent, 60 * 60, ring.latest()->wallMs);
        EXPECT_TRUE(!pts.isEmpty() && pts.first().y() > 4.0, "the oldest surviving record is not one that was evicted");
    }

    // 3. Raw slicing (<= 5 min): one point per record inside the window, x in
    //    seconds since the cutoff, older records skipped.
    {
        CpuHistoryRing ring;
        ring.push(at(kT0 - 70'000, 99.0));   // outside a 60 s window
        ring.push(at(kT0 - 30'000, 10.0));
        ring.push(at(kT0,          20.0));
        const auto pts = ring.series(Field::ProcessPercent, 60, kT0);
        EXPECT_EQ(pts.size(), 2);
        EXPECT_TRUE(pts.size() == 2 && near(pts[0].x(), 30.0) && near(pts[0].y(), 10.0), "first point at 30 s = 10 %");
        EXPECT_TRUE(pts.size() == 2 && near(pts[1].x(), 60.0) && near(pts[1].y(), 20.0), "second point at 60 s = 20 %");
    }

    // 4. Bucketed slicing (1 h): records sharing a 12 s bucket average, placed at
    //    the bucket centre — the rule MemoryHistoryRing pins for the same chart.
    {
        CpuHistoryRing ring;
        const qint64 bucketMs = CpuHistoryRing::bucketMsFor(3600);
        EXPECT_EQ(bucketMs, 12000);
        const qint64 nowAligned = ((kT0 / bucketMs) + 1) * bucketMs;
        const qint64 start = nowAligned - bucketMs;   // the last whole bucket
        ring.push(at(start + 1000, 10.0));
        ring.push(at(start + 2500, 30.0));
        const auto pts = ring.series(Field::ProcessPercent, 3600, nowAligned);
        EXPECT_EQ(pts.size(), 1);
        EXPECT_TRUE(pts.size() == 1 && near(pts[0].y(), 20.0), "two records in one bucket average to 20 %");
        EXPECT_TRUE(pts.size() == 1 && near(pts[0].x(), 3600.0 - 6.0), "bucket point sits at the bucket centre");
    }

    // 5. Tick-lag fields plot only when the meter was read (tickCount > 0).
    {
        CpuHistoryRing ring;
        CpuHistoryRing::Record unread = at(kT0 - 1500, 5.0);
        ring.push(unread);
        CpuHistoryRing::Record read = at(kT0, 5.0);
        read.tickCount = 30;
        read.tickLagMaxMs = 12.5;
        read.tickLagMeanMs = 1.5;
        ring.push(read);
        EXPECT_EQ(ring.series(Field::TickLagMax, 60, kT0).size(), 1);
        EXPECT_TRUE(near(ring.series(Field::TickLagMax, 60, kT0).first().y(), 12.5), "tick lag max plots its value");
        EXPECT_EQ(ring.series(Field::ProcessPercent, 60, kT0).size(), 2);
    }

    // 6. Invalid records (no interval yet) are skipped by every series.
    {
        CpuHistoryRing ring;
        CpuHistoryRing::Record invalid;
        invalid.wallMs = kT0;
        ring.push(invalid);
        EXPECT_EQ(ring.series(Field::ProcessPercent, 60, kT0).size(), 0);
        EXPECT_EQ(ring.topThreadSeries(5, 60, kT0).size(), 0);
    }

    // 7. Top-N by MEAN over the window, absent ticks counting zero; members get
    //    one aligned point per record; ordering highest mean first.
    {
        CpuHistoryRing ring;
        using Share = CpuHistoryRing::ThreadShare;
        // Thread 1 busy on every tick (mean 40); thread 2 hot once (90 on one
        // of three ticks = mean 30); thread 3 steady low (mean 10); thread 4
        // appears once at 3 (mean 1).
        ring.push(at(kT0 - 3000, 20.0, {Share{1, "AudioEngine", 40.0}, Share{3, "Spots", 10.0}, Share{4, "Idle", 3.0}}));
        ring.push(at(kT0 - 1500, 20.0, {Share{1, "AudioEngine", 40.0}, Share{2, "Pan", 90.0}, Share{3, "Spots", 10.0}}));
        ring.push(at(kT0,        20.0, {Share{1, "AudioEngine", 40.0}, Share{3, "Spots", 10.0}}));
        const auto top = ring.topThreadSeries(2, 60, kT0);
        EXPECT_EQ(top.size(), 2);
        if (top.size() == 2) {
            EXPECT_TRUE(top[0].tid == 1 && near(top[0].meanPercentOfCore, 40.0), "AudioEngine leads by mean");
            EXPECT_TRUE(top[1].tid == 2 && near(top[1].meanPercentOfCore, 30.0), "Pan is second by mean (90 once over three ticks)");
            EXPECT_EQ(top[0].points.size(), 3);
            EXPECT_EQ(top[1].points.size(), 3);
            EXPECT_TRUE(near(top[1].points[0].y(), 0.0) && near(top[1].points[1].y(), 90.0) && near(top[1].points[2].y(), 0.0),
                        "an absent tick contributes a zero point, keeping the series aligned");
            EXPECT_TRUE(near(top[0].points[0].x(), top[1].points[0].x()), "members share x positions");
        }
        // Asking for more members than exist returns what exists.
        EXPECT_EQ(ring.topThreadSeries(10, 60, kT0).size(), 4);
        // Ties break by tid so the order does not flicker.
        const auto all = ring.topThreadSeries(10, 60, kT0);
        EXPECT_TRUE(all.size() == 4 && all[2].tid == 3 && all[3].tid == 4, "remaining members ordered by mean then tid");
    }

    // 8. The window, not the newest tick, decides membership: a thread hot on an
    //    old tick outside the window is not a member.
    {
        CpuHistoryRing ring;
        using Share = CpuHistoryRing::ThreadShare;
        ring.push(at(kT0 - 120'000, 50.0, {Share{9, "Old", 100.0}}));
        ring.push(at(kT0, 5.0, {Share{1, "Now", 5.0}}));
        const auto top = ring.topThreadSeries(5, 60, kT0);
        EXPECT_EQ(top.size(), 1);
        EXPECT_TRUE(top.size() == 1 && top[0].tid == 1, "only threads seen inside the window are members");
    }

    if (g_failures) {
        std::fprintf(stderr, "cpu_history_ring_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("cpu_history_ring_test: all checks passed\n");
    return 0;
}
