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
struct ThreadTimes {
    quint64 tid{0};
    QString name;      // kernel-visible name, empty when the thread has none
    quint64 cpuUsecs{0};  // cumulative user + system time since thread start
};

// One thread's share of one core over a sampling interval.
struct ThreadCpuSample {
    quint64 tid{0};
    QString name;
    quint64 cpuUsecs{0};       // cumulative, carried through for a "total" column
    double  cpuPercentOfCore{0.0};  // 0..100 per core; >100 is not possible per thread
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
    // Without this the Threads tab is useless: nothing in AetherSDR sets a
    // kernel thread name today, so every row would read as a bare tid.
    // Truncated to 15 characters on Linux, which is the kernel's limit.
    static void setCurrentThreadName(const char* name);

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
