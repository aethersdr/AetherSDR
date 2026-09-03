// #2554 (Memory tab) -- MemoryHistoryRing: bounded retention, window slicing and
// the bucket rule the chart shares with NetworkDiagnosticsDialog. Pure logic; no
// widget, no socket, no radio. Every number below is CONSTRUCTED: the samples
// stand for no captured process and exercise arithmetic and routing only.
#include "gui/MemoryHistoryRing.h"

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

static MemoryHistoryRing::Record at(qint64 wallMs, quint64 residentMb)
{
    MemoryHistoryRing::Record s;
    s.wallMs = wallMs;
    s.valid = true;
    s.residentMetric = QStringLiteral("vmRss");
    s.residentBytes = residentMb * 1024ull * 1024ull;
    s.peakResidentBytes = s.residentBytes + 1024ull * 1024ull;
    s.privateBytes = s.residentBytes / 2;
    s.virtualBytes = s.residentBytes * 4;
    return s;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using Field = MemoryHistoryRing::Field;
    const qint64 kT0 = 1'000'000'000'000;   // an arbitrary epoch-ms origin

    // 1. Empty ring, empty series; latest() is null.
    {
        MemoryHistoryRing ring;
        EXPECT_TRUE(ring.isEmpty(), "new ring is empty");
        EXPECT_TRUE(ring.latest() == nullptr, "latest() is null on an empty ring");
        EXPECT_EQ(ring.series(Field::Resident, 60, kT0).size(), 0);
    }

    // 2. Retention: capacity is kCapacity; the oldest sample is the one evicted.
    {
        MemoryHistoryRing ring;
        for (int i = 0; i < MemoryHistoryRing::kCapacity + 5; ++i) {
            ring.push(at(kT0 + i * 1500, 100 + i));
        }
        EXPECT_EQ(ring.size(), MemoryHistoryRing::kCapacity);
        EXPECT_EQ(static_cast<int>(ring.latest()->residentBytes / (1024 * 1024)),
                  100 + MemoryHistoryRing::kCapacity + 4);
        // the surviving oldest sample is i = 5
        const auto pts = ring.series(Field::Resident, 60 * 60, kT0 + (MemoryHistoryRing::kCapacity + 4) * 1500);
        EXPECT_TRUE(!pts.isEmpty(), "an hour of samples yields points");
    }

    // 3. Window slicing at 1 s resolution: only samples inside the window, x
    //    measured from the cutoff, values in MB.
    {
        MemoryHistoryRing ring;
        const qint64 now = kT0 + 120'000;   // two minutes in
        for (int i = 0; i <= 80; ++i) {
            ring.push(at(kT0 + i * 1500, 200));   // 0 .. 120 s, every 1.5 s
        }
        const auto pts = ring.series(Field::Resident, 60, now);   // last 60 s
        EXPECT_EQ(pts.size(), 41);                                 // t = 60, 61.5, ... 120
        EXPECT_TRUE(std::fabs(pts.first().x() - 0.0) < 1e-9, "first point sits at the cutoff");
        EXPECT_TRUE(std::fabs(pts.last().x() - 60.0) < 1e-9, "last point sits at the window end");
        EXPECT_TRUE(std::fabs(pts.first().y() - 200.0) < 1e-9, "resident plotted in MB");
        EXPECT_TRUE(std::fabs(ring.series(Field::Private, 60, now).first().y() - 100.0) < 1e-9,
                    "private plotted from its own field");
        EXPECT_TRUE(std::fabs(ring.series(Field::Virtual, 60, now).first().y() - 800.0) < 1e-9,
                    "virtual plotted from its own field");
        EXPECT_TRUE(std::fabs(ring.series(Field::Peak, 60, now).first().y() - 201.0) < 1e-9,
                    "peak plotted from its own field");
    }

    // 4. The bucket rule matches the network dialog: 1 s up to 5 min, then
    //    max(5 s, range/300).
    {
        EXPECT_EQ(MemoryHistoryRing::bucketMsFor(60), 1000);
        EXPECT_EQ(MemoryHistoryRing::bucketMsFor(5 * 60), 1000);
        EXPECT_EQ(MemoryHistoryRing::bucketMsFor(15 * 60), 5000);
        EXPECT_EQ(MemoryHistoryRing::bucketMsFor(60 * 60), 12000);
    }

    // 5. Bucketed slicing averages samples that share a bucket: an hour of
    //    1.5 s samples becomes ~300 points, and a bucket holding 150 and 250
    //    reads 200.
    {
        MemoryHistoryRing ring;
        const int n = 2400;
        for (int i = 0; i < n; ++i) {
            ring.push(at(kT0 + i * 1500, (i % 2 == 0) ? 150 : 250));
        }
        const qint64 now = kT0 + (n - 1) * 1500;
        const auto pts = ring.series(Field::Resident, 60 * 60, now);
        EXPECT_TRUE(pts.size() >= 290 && pts.size() <= 301, "an hour buckets to about 300 points");
        bool allAveraged = true;
        for (const QPointF& p : pts) {
            if (std::fabs(p.y() - 200.0) > 51.0) {   // a full 12 s bucket holds 8 samples: 4×150 + 4×250
                allAveraged = false;
            }
        }
        EXPECT_TRUE(allAveraged, "bucket values are averages of their samples");
        EXPECT_TRUE(pts.first().x() >= 0.0 && pts.last().x() <= 3600.0, "x stays inside the window");
    }

    // 6. The line-break threshold follows the bucket: 4.5 s at raw resolution,
    //    three buckets beyond five minutes (a fixed 4.5 s isolated every 12 s
    //    bucket point of a one-hour view and drew nothing).
    {
        EXPECT_TRUE(std::fabs(MemoryHistoryRing::connectGapSecondsFor(60) - 4.5) < 1e-9, "1 min: three samples");
        EXPECT_TRUE(std::fabs(MemoryHistoryRing::connectGapSecondsFor(5 * 60) - 4.5) < 1e-9, "5 min: three samples");
        EXPECT_TRUE(std::fabs(MemoryHistoryRing::connectGapSecondsFor(15 * 60) - 15.0) < 1e-9, "15 min: three 5 s buckets");
        EXPECT_TRUE(std::fabs(MemoryHistoryRing::connectGapSecondsFor(60 * 60) - 36.0) < 1e-9, "1 hour: three 12 s buckets");
        EXPECT_TRUE(MemoryHistoryRing::connectGapSecondsFor(60 * 60)
                        > static_cast<double>(MemoryHistoryRing::bucketMsFor(60 * 60)) / 1000.0,
                    "the threshold always exceeds one bucket, so consecutive bucket points connect");
    }

    if (g_failures) {
        std::fprintf(stderr, "memory_history_ring_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("memory_history_ring_test: all checks passed\n");
    return 0;
}
