// Tests for the per-thread CPU accounting behind the System Info dialog (#2554).
//
// The sampling maths is pure by design (SystemInfo::cpuPercentBetween takes two
// snapshots and an interval), so the cases that actually bite — a thread that
// appeared mid-interval, a tid reused after a thread exited, a zero-length
// interval — are testable without a process to observe. The enumeration itself
// is checked for the invariants that hold on every platform rather than for
// values that would differ between them.

#include "core/SystemInfo.h"
#include "core/ThreadCpuRing.h"

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

const char* stateName(ThreadRunState state)
{
    switch (state) {
    case ThreadRunState::Unknown:         return "Unknown";
    case ThreadRunState::Running:         return "Running";
    case ThreadRunState::Waiting:         return "Waiting";
    case ThreadRunState::Uninterruptible: return "Uninterruptible";
    case ThreadRunState::Stopped:         return "Stopped";
    case ThreadRunState::Halted:          return "Halted";
    case ThreadRunState::Zombie:          return "Zombie";
    }
    return "?";
}

void testRunState()
{
    // The /proc state character mapping is pure and platform-free, so it is
    // tested on every host rather than only on the kernel that writes it.
    report("'R' is Running",
           SystemInfo::runStateFromProcChar('R') == ThreadRunState::Running);
    report("'S' is Waiting",
           SystemInfo::runStateFromProcChar('S') == ThreadRunState::Waiting);
    report("'I' — the idle sleep variant — is also Waiting",
           SystemInfo::runStateFromProcChar('I') == ThreadRunState::Waiting);
    report("'D' is Uninterruptible",
           SystemInfo::runStateFromProcChar('D') == ThreadRunState::Uninterruptible);
    report("'T' and 't' are both Stopped",
           SystemInfo::runStateFromProcChar('T') == ThreadRunState::Stopped
               && SystemInfo::runStateFromProcChar('t') == ThreadRunState::Stopped);
    report("'Z' is Zombie",
           SystemInfo::runStateFromProcChar('Z') == ThreadRunState::Zombie);

    // The point of the default arm: an unrecognised character must not be
    // mapped to whichever state looks closest. 'X' is dead, which is not the
    // same claim as Halted, and a character the parse never found at all is a
    // failure to read rather than a state.
    report("'X' (dead) is Unknown, not Halted",
           SystemInfo::runStateFromProcChar('X') == ThreadRunState::Unknown);
    report("an absent character is Unknown",
           SystemInfo::runStateFromProcChar('\0') == ThreadRunState::Unknown);
    report("an unrecognised character is Unknown",
           SystemInfo::runStateFromProcChar('?') == ThreadRunState::Unknown);

    // State rides the delta the same way the name does, and it comes from the
    // CURRENT snapshot — what the thread is doing now, not an average.
    {
        ThreadTimes before = makeThread(3, 0);
        before.state = ThreadRunState::Waiting;
        ThreadTimes after = makeThread(3, 1000);
        after.state = ThreadRunState::Running;
        const auto samples = SystemInfo::cpuPercentBetween({before}, {after}, 1000);
        report("the run state is carried through, taken from the newer snapshot",
               !samples.isEmpty() && samples.first().state == ThreadRunState::Running);
    }

    // The live check: this thread is executing enumerateThreads(), so on a
    // platform that can answer it cannot be anything but Running. Windows has
    // no per-thread state to read, and asserting Unknown there is the point —
    // it pins that the column stays honest rather than acquiring a derived
    // value later.
    ThreadRunState own = ThreadRunState::Unknown;
    bool foundOwn = false;
    // Named so the row can be identified; there is no portable "which row am
    // I" other than the name this very file already proves round-trips.
    SystemInfo::setCurrentThreadName("aether-state");
    for (const ThreadTimes& times : SystemInfo::enumerateThreads()) {
        if (times.name.startsWith(QLatin1String("aether-state"))) {
            own = times.state;
            foundOwn = true;
        }
    }
    std::printf("[info] this thread's own reported run state: %s\n", stateName(own));
    report("the calling thread finds its own row", foundOwn);
#if defined(Q_OS_WIN)
    report("Windows reports Unknown rather than a derived value",
           own == ThreadRunState::Unknown);
#else
    report("the thread doing the enumerating reports Running",
           own == ThreadRunState::Running);
#endif
}

ThreadCpuSample makeSample(quint64 tid, double percent)
{
    ThreadCpuSample sample;
    sample.tid = tid;
    sample.cpuPercentOfCore = percent;
    return sample;
}

void testThreadCpuRing()
{
    // A thread seen for the first time has no series, so the sparkline draws
    // nothing rather than a flat line at zero — which would read as an idle
    // thread rather than an unknown one.
    {
        ThreadCpuRing ring;
        report("an unseen thread has an empty series", ring.seriesFor(1).isEmpty());
        report("an unseen thread peaks at 0", qAbs(ring.peakFor(1)) < 0.001);
    }

    // Order is what the sparkline draws, so it is asserted rather than assumed.
    {
        ThreadCpuRing ring;
        ring.update({makeSample(1, 10.0)});
        ring.update({makeSample(1, 20.0)});
        ring.update({makeSample(1, 15.0)});
        const QVector<double> series = ring.seriesFor(1);
        report("samples accumulate oldest-first",
               series.size() == 3 && qAbs(series.first() - 10.0) < 0.001
                   && qAbs(series.last() - 15.0) < 0.001);
        report("peak is the highest reading in the window",
               qAbs(ring.peakFor(1) - 20.0) < 0.001);
    }

    // The window is bounded, and the sample that falls off the end takes its
    // peak with it: this is the difference between "peak in the last minute"
    // and "peak ever", and only the first is what the column claims.
    {
        ThreadCpuRing ring;
        ring.update({makeSample(1, 99.0)});          // the spike
        for (int i = 0; i < ThreadCpuRing::kSamples; ++i) {
            ring.update({makeSample(1, 1.0)});       // push it out
        }
        const QVector<double> series = ring.seriesFor(1);
        report("the window never exceeds kSamples", series.size() == ThreadCpuRing::kSamples);
        report("a reading older than the window stops counting toward peak",
               qAbs(ring.peakFor(1) - 1.0) < 0.001);
    }

    // Exactly at the boundary, the oldest reading is still inside.
    {
        ThreadCpuRing ring;
        ring.update({makeSample(1, 99.0)});
        for (int i = 0; i < ThreadCpuRing::kSamples - 1; ++i) {
            ring.update({makeSample(1, 1.0)});
        }
        report("a full-but-not-overflowing window keeps its oldest reading",
               ring.seriesFor(1).size() == ThreadCpuRing::kSamples
                   && qAbs(ring.peakFor(1) - 99.0) < 0.001);
    }

    // Retirement. Without it the ring would hold a reading for every thread
    // that had ever existed, and peak would keep reporting the high-water mark
    // of a thread that exited long ago.
    {
        ThreadCpuRing ring;
        ring.update({makeSample(1, 50.0), makeSample(2, 60.0)});
        report("both threads are tracked", ring.trackedThreads() == 2);
        ring.update({makeSample(1, 10.0)});
        report("a thread absent from the newest sample is retired",
               ring.trackedThreads() == 1 && ring.seriesFor(2).isEmpty());
        report("its peak goes with it", qAbs(ring.peakFor(2)) < 0.001);
        report("the surviving thread keeps its history",
               ring.seriesFor(1).size() == 2 && qAbs(ring.peakFor(1) - 50.0) < 0.001);
    }

    // clear() is what runs when the dialog is hidden: a peak spanning a gap in
    // which nothing was sampled would describe a minute nobody observed.
    {
        ThreadCpuRing ring;
        ring.update({makeSample(1, 80.0)});
        ring.clear();
        report("clear() drops every thread",
               ring.trackedThreads() == 0 && ring.seriesFor(1).isEmpty()
                   && qAbs(ring.peakFor(1)) < 0.001);
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

void testQtNamesItsOwnThreads()
{
    // Does Qt already give a started QThread a KERNEL-visible name from its
    // objectName? AetherSDR sets objectName on its worker threads
    // (FlexBackend.cpp:46 "PanadapterStream", MainWindow.cpp:1230 "AudioEngine"),
    // so if Qt propagates that to the OS, the Threads tab has real names with no
    // work from us — and a naming survey would be redundant. Measured rather
    // than assumed, because the answer decides a whole commit's worth of scope.
    QThread thread;
    thread.setObjectName(QStringLiteral("aether-qtprobe"));

    QString observed;
    QObject context;
    context.moveToThread(&thread);
    QObject::connect(&thread, &QThread::started, &context, [&observed]() {
        for (const ThreadTimes& times : SystemInfo::enumerateThreads()) {
            if (times.tid == 0) {
                continue;
            }
            // Identify our own row by naming nothing and matching the name Qt
            // may have set.
            if (times.name.startsWith(QLatin1String("aether-qtprobe"))) {
                observed = times.name;
            }
        }
        QThread::currentThread()->quit();
    });
    thread.start();
    thread.wait(5000);

    std::printf("[info] Qt-assigned kernel thread name: \"%s\"\n",
                observed.toUtf8().constData());
    report("Qt propagates QThread::objectName to the kernel thread name",
           !observed.isEmpty());
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testPercentMaths();
    testRunState();
    testThreadCpuRing();
    testEnumeration();
    testNaming();
    testQtNamesItsOwnThreads();
    std::printf("%s\n", g_failures == 0 ? "system_info_test: all passed"
                                        : "system_info_test: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
