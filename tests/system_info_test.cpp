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
#include <QFile>
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

// Runs body on a freshly started QThread and blocks until it returns. The
// checks that name a thread and then look for that name in the kernel's
// thread table must run OFF the main thread: on Linux the main thread is the
// thread-group leader, whose kernel name is the process name that ps, top and
// pgrep -x match on, and setCurrentThreadName() deliberately leaves it alone
// (ThreadName.cpp). A worker is also the path the helper's real users take —
// IambicKeyer, CwxLocalKeyer, SystemInfoCollector, the RtMidi callback.
//
// quit() before exec() is fine: QThread::exec() returns at once when exit()
// has already been called, so the thread ends as soon as body does.
template <typename Body>
void runOnWorkerThread(Body body)
{
    QThread thread;
    QObject context;
    context.moveToThread(&thread);
    QObject::connect(&thread, &QThread::started, &context, [&body]() {
        body();
        QThread::currentThread()->quit();
    });
    thread.start();
    thread.wait(5000);
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

    // The live check: the thread executing enumerateThreads() cannot, on a
    // platform that can answer, be anything but Running. Windows has no
    // per-thread state to read, and asserting Unknown there is the point — it
    // pins that the column stays honest rather than acquiring a derived value
    // later. Runs on a worker because the row is found by name, and naming
    // the main thread does not reach the kernel on Linux (runOnWorkerThread).
    ThreadRunState own = ThreadRunState::Unknown;
    bool foundOwn = false;
    runOnWorkerThread([&own, &foundOwn]() {
        // Named so the row can be identified; there is no portable "which row
        // am I" other than the name this very file already proves round-trips.
        SystemInfo::setCurrentThreadName("aether-state");
        for (const ThreadTimes& times : SystemInfo::enumerateThreads()) {
            if (times.name.startsWith(QLatin1String("aether-state"))) {
                own = times.state;
                foundOwn = true;
            }
        }
    });
    std::printf("[info] the worker's own reported run state: %s\n", stateName(own));
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

void testBusiestAndThreshold()
{
    // -1 means "there was nothing to read", and it is the only thing that does.
    report("no samples has no busiest thread",
           SystemInfo::busiestThreadIndex({}) == -1);

    // All idle still HAS a busiest thread. "Nothing is busy" is a reading; the
    // summary line should name a thread at 0 % rather than fall back to a dash.
    {
        const QVector<ThreadCpuSample> samples{makeSample(1, 0.0), makeSample(2, 0.0)};
        report("an all-idle table still has a busiest thread",
               SystemInfo::busiestThreadIndex(samples) == 0);
    }

    {
        const QVector<ThreadCpuSample> samples{
            makeSample(1, 3.0), makeSample(2, 91.5), makeSample(3, 12.0)};
        report("the busiest thread is found wherever it sits",
               SystemInfo::busiestThreadIndex(samples) == 1);
    }

    // Ties keep the first. The table redraws every 1.5 s, and alternating
    // between two equally idle threads would make the summary line flicker.
    {
        const QVector<ThreadCpuSample> samples{
            makeSample(1, 42.0), makeSample(2, 42.0)};
        report("a tie resolves to the lower index",
               SystemInfo::busiestThreadIndex(samples) == 0);
    }

    // The crossing latch. A thread pinned above the line is one event, not one
    // per sample — this is what separates thresholdExceeded from sampleReady.
    const double t = 90.0;
    report("rising through the threshold is a crossing",
           SystemInfo::crossedThreshold(50.0, 95.0, t));
    report("staying above it is not another crossing",
           !SystemInfo::crossedThreshold(95.0, 97.0, t));
    report("falling back below it is not a crossing",
           !SystemInfo::crossedThreshold(95.0, 20.0, t));
    report("rising again after coming back down is a fresh crossing",
           SystemInfo::crossedThreshold(20.0, 95.0, t));

    // "Exceeds 90%" is the criterion's own wording, so exactly 90 is not
    // across the line. Pinned because it is the kind of boundary a later edit
    // would flip without noticing.
    report("exactly at the threshold has not exceeded it",
           !SystemInfo::crossedThreshold(50.0, 90.0, t));
    report("a hair above it has",
           SystemInfo::crossedThreshold(50.0, 90.001, t));
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
    // The Qt half does not depend on which thread asks, so it is checked on
    // the main thread, where the kernel half is platform-dependent (below).
    SystemInfo::setCurrentThreadName("aether-sysinfo");
    report("naming sets Qt's objectName too",
           QThread::currentThread()->objectName() == QLatin1String("aether-sysinfo"));

    // The kernel name is what the Threads tab shows, so prove it round-trips
    // rather than trusting that the syscall was issued.
    bool foundNamed = false;
    runOnWorkerThread([&foundNamed]() {
        SystemInfo::setCurrentThreadName("aether-worker");
        for (const ThreadTimes& times : SystemInfo::enumerateThreads()) {
            // Linux truncates to 15 characters, so compare on the prefix that
            // survives on every platform.
            if (times.name.startsWith(QLatin1String("aether-worker"))) {
                foundNamed = true;
            }
        }
    });
    report("the kernel-visible name round-trips through enumerateThreads()", foundNamed);

    // An empty or null name is a no-op rather than a crash or a cleared name.
    SystemInfo::setCurrentThreadName(nullptr);
    SystemInfo::setCurrentThreadName("");
    report("a null or empty name leaves the existing name alone",
           QThread::currentThread()->objectName() == QLatin1String("aether-sysinfo"));

#if defined(Q_OS_LINUX)
    // The main thread is the thread-group leader, and its comm IS the process
    // name: renaming it renamed the whole process for ps, top, pgrep -x and
    // killall (measured on this branch before the guard: `pgrep -x AetherSDR`
    // found nothing once main.cpp named its thread AetherSDR-GUI). Naming the
    // leader must therefore reach Qt and stop short of the kernel.
    const auto readComm = []() {
        QFile comm(QStringLiteral("/proc/self/comm"));
        return comm.open(QIODevice::ReadOnly) ? QString::fromUtf8(comm.readAll()).trimmed()
                                              : QString();
    };
    const QString before = readComm();
    SystemInfo::setCurrentThreadName("aether-leader");
    report("naming the thread-group leader leaves /proc/self/comm alone",
           !before.isEmpty() && readComm() == before);
    report("naming the thread-group leader still sets Qt's objectName",
           QThread::currentThread()->objectName() == QLatin1String("aether-leader"));
#endif
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
    testBusiestAndThreshold();
    testEnumeration();
    testNaming();
    testQtNamesItsOwnThreads();
    std::printf("%s\n", g_failures == 0 ? "system_info_test: all passed"
                                        : "system_info_test: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
