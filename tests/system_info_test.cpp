// Tests for the per-thread CPU accounting behind the System Info dialog (#2554).
//
// The sampling maths is pure by design (SystemInfo::cpuPercentBetween takes two
// snapshots and an interval), so the cases that actually bite — a thread that
// appeared mid-interval, a tid reused after a thread exited, a zero-length
// interval — are testable without a process to observe. The enumeration itself
// is checked for the invariants that hold on every platform rather than for
// values that would differ between them.

#include "core/SystemInfo.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QThread>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void report(const char* what, bool ok)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

ThreadTimes makeThread(quint64 tid, quint64 cpuUsecs, const char* name = "")
{
    ThreadTimes times;
    times.tid = tid;
    times.cpuUsecs = cpuUsecs;
    times.name = QString::fromUtf8(name);
    return times;
}

double percentFor(const QVector<ThreadCpuSample>& samples, quint64 tid)
{
    for (const ThreadCpuSample& sample : samples) {
        if (sample.tid == tid) {
            return sample.cpuPercentOfCore;
        }
    }
    return -1.0;
}

void testPercentMaths()
{
    // Half a core over the interval reads as 50 %, not 50 % of the machine.
    {
        const QVector<ThreadTimes> before{makeThread(1, 1000000)};
        const QVector<ThreadTimes> after{makeThread(1, 1500000)};
        const auto samples = SystemInfo::cpuPercentBetween(before, after, 1000000);
        report("half a core over the interval reads 50%",
               qAbs(percentFor(samples, 1) - 50.0) < 0.001);
    }

    // A fully busy thread is 100 % of ONE core — the number must not be
    // divided down by the core count, which is the masking this whole feature
    // exists to undo.
    {
        const QVector<ThreadTimes> before{makeThread(1, 0)};
        const QVector<ThreadTimes> after{makeThread(1, 2000000)};
        const auto samples = SystemInfo::cpuPercentBetween(before, after, 2000000);
        report("a saturated thread reads 100%",
               qAbs(percentFor(samples, 1) - 100.0) < 0.001);
    }

    // Started during the interval: charging its whole lifetime to this one
    // interval would show a brand-new thread as impossibly hot.
    {
        const QVector<ThreadTimes> before{makeThread(1, 0)};
        const QVector<ThreadTimes> after{makeThread(1, 0), makeThread(2, 900000)};
        const auto samples = SystemInfo::cpuPercentBetween(before, after, 1000000);
        report("a thread new this interval reports 0%",
               qAbs(percentFor(samples, 2)) < 0.001);
        report("its cumulative total is still carried", samples.size() == 2);
    }

    // Exited during the interval: dropped rather than reported as idle.
    {
        const QVector<ThreadTimes> before{makeThread(1, 0), makeThread(2, 0)};
        const QVector<ThreadTimes> after{makeThread(1, 100000)};
        const auto samples = SystemInfo::cpuPercentBetween(before, after, 1000000);
        report("a thread that exited is dropped", samples.size() == 1);
    }

    // Counter apparently moving backwards — tid reuse — must clamp, never go
    // negative. A negative percentage would sort to the top of a table sorted
    // by CPU descending.
    {
        const QVector<ThreadTimes> before{makeThread(1, 5000000)};
        const QVector<ThreadTimes> after{makeThread(1, 1000)};
        const auto samples = SystemInfo::cpuPercentBetween(before, after, 1000000);
        report("a backwards counter clamps to 0, never negative",
               percentFor(samples, 1) >= 0.0 && percentFor(samples, 1) < 0.001);
    }

    // A zero-length interval must not divide by zero.
    {
        const QVector<ThreadTimes> before{makeThread(1, 0)};
        const QVector<ThreadTimes> after{makeThread(1, 500000)};
        const auto samples = SystemInfo::cpuPercentBetween(before, after, 0);
        report("a zero-length interval yields 0%, not a division by zero",
               qAbs(percentFor(samples, 1)) < 0.001);
    }

    // Names ride along so the table has something to show.
    {
        const QVector<ThreadTimes> before{makeThread(7, 0, "PanadapterStream")};
        const QVector<ThreadTimes> after{makeThread(7, 10, "PanadapterStream")};
        const auto samples = SystemInfo::cpuPercentBetween(before, after, 1000);
        report("the thread name is carried through",
               !samples.isEmpty() && samples.first().name == QLatin1String("PanadapterStream"));
    }
}

void testEnumeration()
{
    const QVector<ThreadTimes> threads = SystemInfo::enumerateThreads();
    report("enumerateThreads() sees at least this thread", !threads.isEmpty());

    bool everyTidSet = true;
    bool everyTidUnique = true;
    QSet<quint64> seen;
    for (const ThreadTimes& times : threads) {
        if (times.tid == 0) {
            everyTidSet = false;
        }
        if (seen.contains(times.tid)) {
            everyTidUnique = false;
        }
        seen.insert(times.tid);
    }
    report("every enumerated thread has a tid", everyTidSet);
    report("tids are unique within one snapshot", everyTidUnique);

    // Two snapshots in a row must not report time running backwards for a
    // thread that stayed alive — the property cpuPercentBetween relies on.
    const QVector<ThreadTimes> again = SystemInfo::enumerateThreads();
    QHash<quint64, quint64> firstByTid;
    for (const ThreadTimes& times : threads) {
        firstByTid.insert(times.tid, times.cpuUsecs);
    }
    bool monotonic = true;
    for (const ThreadTimes& times : again) {
        const auto found = firstByTid.constFind(times.tid);
        if (found != firstByTid.constEnd() && times.cpuUsecs < *found) {
            monotonic = false;
        }
    }
    report("cumulative CPU time does not go backwards across snapshots", monotonic);
}

void testNaming()
{
    SystemInfo::setCurrentThreadName("aether-sysinfo");
    report("naming sets Qt's objectName too",
           QThread::currentThread()->objectName() == QLatin1String("aether-sysinfo"));

    // The kernel name is what the Threads tab shows, so prove it round-trips
    // rather than trusting that the syscall was issued.
    bool foundNamed = false;
    for (const ThreadTimes& times : SystemInfo::enumerateThreads()) {
        // Linux truncates to 15 characters, so compare on the prefix that
        // survives on every platform.
        if (times.name.startsWith(QLatin1String("aether-sysinfo"))) {
            foundNamed = true;
        }
    }
    report("the kernel-visible name round-trips through enumerateThreads()", foundNamed);

    // An empty or null name is a no-op rather than a crash or a cleared name.
    SystemInfo::setCurrentThreadName(nullptr);
    SystemInfo::setCurrentThreadName("");
    report("a null or empty name leaves the existing name alone",
           QThread::currentThread()->objectName() == QLatin1String("aether-sysinfo"));
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testPercentMaths();
    testEnumeration();
    testNaming();
    std::printf("%s\n", g_failures == 0 ? "system_info_test: all passed"
                                        : "system_info_test: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
