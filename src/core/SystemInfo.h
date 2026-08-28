#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

namespace AetherSDR {

// Per-thread CPU accounting for the System Info dialog (#2554).
//
// The diagnostic this exists for: AetherSDR's characteristic performance
// failure is ONE thread saturating ONE core while the others idle, which the
// status bar's single system-wide percentage hides completely (#2545). Seeing
// which thread is hot is the whole point, so the unit here is per-thread, and
// the percentage is "of one core" rather than "of the machine".
//
// Cumulative counters, not instantaneous readings. macOS exposes an
// instantaneous `cpu_usage` field, but Linux and Windows only offer cumulative
// user/system time, and mixing the two would make the three platforms disagree
// about what the same number means. Everything here reports cumulative
// microseconds; percentages are derived by cpuPercentBetween().
// A thread's run state, as the kernel reports it.
//
// An enum handed up from the platform layer, with the display wording owned by
// the GUI, because the two platforms that can answer use different vocabularies
// — mach's TH_STATE_* constants against the single character in
// /proc/<tid>/stat — and letting each hand back its own words would make one
// column mean different things on different machines.
//
// Windows reports Unknown. THREADENTRY32 carries no state field and
// GetThreadTimes returns times only, so there is nothing documented to read.
// Deriving one from "the counter did not advance this interval" was considered
// and rejected: that is a computed guess wearing a kernel state's name, and it
// would make the column mean a third thing on a third platform.
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
    // Every thread in THIS process with its cumulative CPU time and kernel
    // name. Empty on failure rather than partially populated — a half-read
    // thread table is worse than none for a diagnostic.
    //
    // macOS: task_threads() + thread_info(THREAD_EXTENDED_INFO), which carries
    //   pth_name alongside the times. Every returned port is deallocated and
    //   the array vm_deallocate()d — see the RAII wrapper in the .cpp; leaking
    //   mach ports from a function that runs every 1.5 s would exhaust the
    //   port table.
    // Linux: /proc/self/task/<tid>/stat fields 14/15, converted with
    //   sysconf(_SC_CLK_TCK) — never a hard-coded 100, which is merely the
    //   common value. Name from /proc/self/task/<tid>/comm.
    // Windows: CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD) filtered to this
    //   process, GetThreadTimes(), name from GetThreadDescription().
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
};

} // namespace AetherSDR
