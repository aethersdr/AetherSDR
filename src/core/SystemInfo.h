#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace AetherSDR {

// Per-thread CPU accounting for System Info (#2554). The typical failure is
// one thread saturating one core (#2545), so percentages are "of one core".
// All values are cumulative microseconds (Linux/Windows offer nothing
// instantaneous); cpuPercentBetween() derives percentages.
//
// Thread run state as a platform-neutral enum; the GUI owns the wording.
// Windows reports Unknown: THREADENTRY32/GetThreadTimes expose no state.
enum class ThreadRunState {
    Unknown = 0,      // the platform cannot say
    Running,          // on a core now
    Waiting,          // sleeping, interruptibly
    Uninterruptible,  // blocked in the kernel, not interruptible
    Stopped,          // suspended or traced
    Halted,           // stopped at a clean point (macOS TH_STATE_HALTED)
    Zombie,           // exited, not yet reaped (Linux 'Z')
};

struct ThreadTimes {
    quint64 tid{0};
    QString name;      // kernel-visible name, empty when the thread has none
    quint64 cpuUsecs{0};  // cumulative user + system time since thread start
    ThreadRunState state{ThreadRunState::Unknown};
};

// One thread's share of one core over a sampling interval.
struct ThreadCpuSample {
    quint64 tid{0};
    QString name;
    quint64 cpuUsecs{0};       // cumulative, carried through for a "total" column
    double  cpuPercentOfCore{0.0};  // 0..100 per core; >100 is not possible per thread
    ThreadRunState state{ThreadRunState::Unknown};
};

class SystemInfo {
public:
    // Every thread in this process with cumulative CPU time and kernel name;
    // empty on any failure (never partial).
    // macOS: task_threads() + THREAD_EXTENDED_INFO; every port is deallocated
    //   (RAII in the .cpp) — this runs every 1.5 s and would exhaust the table.
    // Linux: /proc/self/task/<tid>/stat fields 14/15 via sysconf(_SC_CLK_TCK)
    //   (not a hard-coded 100); name from .../comm.
    // Windows: Toolhelp32 TH32CS_SNAPTHREAD, GetThreadTimes, GetThreadDescription.
    static QVector<ThreadTimes> enumerateThreads();

    // Name the CALLING thread, for both the kernel (ps -L, Instruments, perf,
    // Windows Performance Analyzer) and Qt (QThread::objectName()), so every
    // tool agrees. Call once at the top of a worker's entry point.
    //
    // Qt already names the QThreads it starts from their objectName. This is
    // for the threads that never pass through QThread::start() — the main
    // thread, raw std::thread workers, framework callback threads — which
    // otherwise read as unnamed rows in the Threads tab.
    // Truncated to 15 characters on Linux, which is the kernel's limit.
    static void setCurrentThreadName(const char* name);

    // Index of the thread using the most of one core, or -1 when there are no
    // samples. Ties resolve to the lowest index so a table that redraws every
    // 1.5 s does not flicker between two equally idle threads.
    //
    // A vector of all-zero samples still has a busiest thread: it returns 0
    // rather than -1, because "nothing is busy" is a reading, not an absence of
    // one. -1 means only that there was nothing to read.
    static int busiestThreadIndex(const QVector<ThreadCpuSample>& samples);

    // Did this reading cross UP through the threshold? Acceptance criterion 3
    // asks for an alert when a thread "exceeds 90% of one core", and a crossing
    // is the event — a thread that sits at 95 % for a minute crossed once, not
    // forty times.
    //
    // Strictly greater, which is what "exceeds" says: exactly at the threshold
    // is not across it. Pure and separate from the collector so the latch is
    // testable without a running thread.
    static bool crossedThreshold(double previousPercent, double currentPercent,
                                 double threshold);

    // The state character in /proc/<pid>/task/<tid>/stat mapped to the shared
    // vocabulary. Pure and platform-free so it is testable on every host, not
    // only the one whose kernel writes the character. Anything unrecognised —
    // including 'X' (dead), which is not the same claim as Halted — is Unknown
    // rather than a nearest guess.
    static ThreadRunState runStateFromProcChar(char state);

    // Percentage of one core each thread used between two snapshots. Pure:
    // no syscalls, no clock reads, so the sampling maths is testable without a
    // process to observe.
    //
    // Threads absent from `previous` (started during the interval) report 0 %
    // rather than charging their whole lifetime to one interval. Threads absent
    // from `current` are dropped. A counter that appears to move backwards —
    // tid reuse, or a platform quirk — clamps to 0 instead of producing a
    // negative percentage. elapsedUsecs == 0 yields all-zero percentages.
    static QVector<ThreadCpuSample> cpuPercentBetween(const QVector<ThreadTimes>& previous,
                                                      const QVector<ThreadTimes>& current,
                                                      quint64 elapsedUsecs);

    // Whole-process cumulative CPU (user + system) in µs, or nullopt. Includes
    // exited threads, which summing per-thread deltas misses (short-lived workers
    // vanish between snapshots). Same source as the status bar's CPU label.
    // POSIX: getrusage(RUSAGE_SELF); Windows: GetProcessTimes (100 ns → µs).
    static std::optional<quint64> processCpuUsecs();

    // The process's share of the WHOLE machine over an interval, 0..100, from
    // two readings of processCpuUsecs(): (current − previous) / elapsed, then
    // divided by the core count — the same divisor the status bar's CPU label
    // uses (idealThreadCount()), so the Overview's "CPU Total" card and the
    // footer read the same thing. Pure so the arithmetic is testable without
    // a process to observe. Clamped to 100 (the two clocks are read a few
    // microseconds apart). Zero for a zero interval, a non-positive core
    // count, or a counter that appears to run backwards.
    static double processPercentOfCapacity(quint64 previousCpuUsecs, quint64 currentCpuUsecs,
                                           quint64 elapsedUsecs, int coreCount);

    // Which colour band an Overview card sits in for a reading (#2554: "yellow
    // ≥50%, red ≥80%" and the like). Inclusive at both lines, which is what the
    // issue's "≥" says; Danger wins when both are met. Pure so the table of
    // thresholds can be pinned without a widget.
    enum class CardLevel { Normal, Warning, Danger };
    static CardLevel cardLevel(double value, double warningAt, double dangerAt);
};

} // namespace AetherSDR
