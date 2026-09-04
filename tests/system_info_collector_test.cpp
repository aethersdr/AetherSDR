// #2554 (Memory tab) -- SystemInfoCollector publishes a process-memory sample on
// every tick, including the first, through a queued signal that survives the
// thread hop. Runs the real wiring (parentless collector moved onto a QThread,
// init() on QThread::started, the 1.5 s timer) rather than calling into the
// sampler directly, because the metatype registration and the queued copy are
// exactly what a unit that bypassed the thread would not exercise.
//
// The reading is MEASURED on this test process (a live ProcessMemorySnapshot,
// the precedent memory_telemetry_test sets); nothing here is constructed. No
// socket, no radio, no widget.
#include "core/SystemInfoCollector.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSet>
#include <QSignalSpy>
#include <QString>
#include <QThread>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_TRUE(cond, what) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QThread worker;
    worker.setObjectName(QStringLiteral("SystemInfoCollectorTest"));
    auto* collector = new SystemInfoCollector;   // parentless, moved to the worker
    collector->moveToThread(&worker);
    QObject::connect(&worker, &QThread::finished, collector, &QObject::deleteLater);
    QObject::connect(&worker, &QThread::started, collector, &SystemInfoCollector::init);

    QSignalSpy memorySpy(collector, &SystemInfoCollector::memorySampleReady);
    EXPECT_TRUE(memorySpy.isValid(), "memorySampleReady is a registered, connectable signal");
    QSignalSpy cpuSpy(collector, &SystemInfoCollector::cpuSampleReady);
    EXPECT_TRUE(cpuSpy.isValid(), "cpuSampleReady is a registered, connectable signal");

    const qint64 before = QDateTime::currentMSecsSinceEpoch();
    worker.start();

    // 1. The first tick already carries a memory sample: 1.5 s cadence, so
    //    one sample well inside 5 s means the tick fired and the queued copy
    //    reached this thread.
    const bool arrived = memorySpy.wait(5000);
    EXPECT_TRUE(arrived, "a memory sample arrives on the GUI thread within 5 s");

    if (arrived) {
        const auto sample = memorySpy.takeFirst().at(0).value<MemorySample>();
        const qint64 after = QDateTime::currentMSecsSinceEpoch();

        // 2. Timestamped by the collector, between start and receipt.
        EXPECT_TRUE(sample.wallMs >= before && sample.wallMs <= after,
                    "wallMs is a capture-time wall clock reading");

        // 3. A live process has a resident set; the metric name says which
        //    kind, and it is one of the names ProcessMemorySnapshot uses.
        static const QSet<QString> kMetrics{
            QStringLiteral("physicalFootprint"), QStringLiteral("workingSet"),
            QStringLiteral("vmRss"), QStringLiteral("unsupported")};
        EXPECT_TRUE(kMetrics.contains(sample.residentMetric),
                    "residentMetric is one of the snapshot's platform names");
        if (sample.residentMetric != QStringLiteral("unsupported")) {
            EXPECT_TRUE(sample.valid, "a supported platform reports a valid sample");
            EXPECT_TRUE(sample.residentBytes > 0, "resident bytes are non-zero on a live process");
            // Like with like only: on macOS peak is resident_size_peak while resident
            // is phys_footprint — two accountings with no ordering between them —
            // so the check is skipped there; workingSet and VmRSS compare with their
            // own peaks.
            if (sample.residentMetric != QStringLiteral("physicalFootprint")) {
                EXPECT_TRUE(sample.peakResidentBytes >= sample.residentBytes,
                            "peak resident is at least the current resident set");
            }
        }
    }

    // 4. The Overview's process-level reading arrives on the first tick too:
    //    init() seeds the previous snapshot, so the first tick already has an
    //    interval. Derived from the same samples the table gets, and carrying
    //    the footer's divisor.
    const bool cpuArrived = !cpuSpy.isEmpty() || cpuSpy.wait(8000);
    EXPECT_TRUE(cpuArrived, "a CPU sample arrives on the GUI thread within 8 s");
    if (cpuArrived) {
        const auto cpu = cpuSpy.takeFirst().at(0).value<CpuSample>();
        EXPECT_TRUE(cpu.coreCount == QThread::idealThreadCount(),
                    "coreCount is idealThreadCount(), the status bar's own divisor");
        EXPECT_TRUE(cpu.processPercentOfCapacity >= 0.0 && cpu.processPercentOfCapacity <= 100.0,
                    "process percent of capacity is within 0..100");
        EXPECT_TRUE(cpu.busiestPercentOfCore >= 0.0 && cpu.busiestPercentOfCore <= 100.0,
                    "busiest thread percent of one core is within 0..100");
        EXPECT_TRUE(cpu.wallMs >= before, "wallMs is a capture-time wall clock reading");
        bool allBusy = true;
        for (const ThreadCpuSample& t : cpu.busyThreads) {
            allBusy = allBusy && t.cpuPercentOfCore > 0.0;
        }
        EXPECT_TRUE(allBusy, "busyThreads carries only threads with a non-zero share");
    }

    // 5. Orderly shutdown on the worker thread (the timer's owner), then quit.
    QMetaObject::invokeMethod(collector, "shutdown", Qt::BlockingQueuedConnection);
    worker.quit();
    EXPECT_TRUE(worker.wait(5000), "worker thread exits after quit()");

    if (g_failures) {
        std::fprintf(stderr, "system_info_collector_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("system_info_collector_test: all checks passed\n");
    return 0;
}
