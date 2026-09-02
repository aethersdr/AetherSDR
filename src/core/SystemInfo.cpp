#include "SystemInfo.h"

#include "ThreadName.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QList>
#include <QThread>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#elif defined(Q_OS_MAC)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <pthread.h>
#elif defined(Q_OS_LINUX)
#include <pthread.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace AetherSDR {

namespace {

#if defined(Q_OS_MAC)
// thread_extended_info's pth_run_state, in the shared vocabulary. The constants
// are TH_STATE_* from <mach/thread_info.h>; anything outside that set is Unknown
// rather than mapped to whichever neighbour looks closest.
ThreadRunState runStateFromMach(int machRunState)
{
    switch (machRunState) {
    case TH_STATE_RUNNING:         return ThreadRunState::Running;
    case TH_STATE_STOPPED:         return ThreadRunState::Stopped;
    case TH_STATE_WAITING:         return ThreadRunState::Waiting;
    case TH_STATE_UNINTERRUPTIBLE: return ThreadRunState::Uninterruptible;
    case TH_STATE_HALTED:          return ThreadRunState::Halted;
    default:                       return ThreadRunState::Unknown;
    }
}

// Owns what task_threads() hands back. Each element is a send right this
// process now holds, and the array itself is vm_allocate()d memory: both must
// be released or a function that runs every 1.5 s leaks the port table shut.
// A wrapper rather than a careful early-return, so no future edit can add a
// return path that forgets.
class MachThreadArray {
public:
    MachThreadArray() = default;
    ~MachThreadArray()
    {
        for (mach_msg_type_number_t i = 0; i < m_count; ++i) {
            mach_port_deallocate(mach_task_self(), m_threads[i]);
        }
        if (m_threads != nullptr) {
            vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(m_threads),
                          m_count * sizeof(thread_act_t));
        }
    }
    MachThreadArray(const MachThreadArray&) = delete;
    MachThreadArray& operator=(const MachThreadArray&) = delete;

    bool acquire()
    {
        return task_threads(mach_task_self(), &m_threads, &m_count) == KERN_SUCCESS;
    }
    mach_msg_type_number_t count() const { return m_count; }
    thread_act_t at(mach_msg_type_number_t i) const { return m_threads[i]; }

private:
    thread_act_array_t     m_threads{nullptr};
    mach_msg_type_number_t m_count{0};
};
#endif

#if defined(Q_OS_LINUX)
// utime and stime are fields 14 and 15 of /proc/<pid>/stat, but the comm field
// is parenthesised and may itself contain spaces and parentheses, so tokenising
// the whole line is wrong. Everything after the LAST ')' is fixed-width:
// state(3) ppid(4) pgrp(5) session(6) tty_nr(7) tpgid(8) flags(9) minflt(10)
// cminflt(11) majflt(12) cmajflt(13) utime(14) stime(15) — i.e. indices 11 and
// 12 counting from the token after ')'. The state character is field 3, which
// is index 0 of that same list, so it comes out of the parse already done
// rather than needing a second read of the file.
bool parseProcStatFields(const QByteArray& stat, quint64* ticks, char* state)
{
    const int close = stat.lastIndexOf(')');
    if (close < 0) {
        return false;
    }
    const QList<QByteArray> fields =
        stat.mid(close + 1).simplified().split(' ');
    if (fields.size() < 13) {
        return false;
    }
    bool okUser = false;
    bool okSystem = false;
    const quint64 utime = fields.at(11).toULongLong(&okUser);
    const quint64 stime = fields.at(12).toULongLong(&okSystem);
    if (!okUser || !okSystem) {
        return false;
    }
    *ticks = utime + stime;
    *state = fields.at(0).isEmpty() ? '\0' : fields.at(0).at(0);
    return true;
}
#endif

}  // namespace

QVector<ThreadTimes> SystemInfo::enumerateThreads()
{
    QVector<ThreadTimes> result;

#if defined(Q_OS_MAC)
    MachThreadArray threads;
    if (!threads.acquire()) {
        return result;
    }
    result.reserve(static_cast<int>(threads.count()));
    for (mach_msg_type_number_t i = 0; i < threads.count(); ++i) {
        ThreadTimes times;

        // THREAD_EXTENDED_INFO carries the name and the times together, so the
        // common case is one call per thread rather than one for each.
        thread_extended_info_data_t extended{};
        mach_msg_type_number_t extendedCount = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads.at(i), THREAD_EXTENDED_INFO,
                        reinterpret_cast<thread_info_t>(&extended),
                        &extendedCount) != KERN_SUCCESS) {
            continue;
        }
        // pth_user_time / pth_system_time are nanoseconds here, unlike the
        // seconds+microseconds pair THREAD_BASIC_INFO reports.
        times.cpuUsecs = (static_cast<quint64>(extended.pth_user_time)
                          + static_cast<quint64>(extended.pth_system_time)) / 1000ull;
        times.name = QString::fromUtf8(extended.pth_name);
        times.state = runStateFromMach(extended.pth_run_state);

        // The mach port is not a thread id and is not stable across calls; the
        // 64-bit id from THREAD_IDENTIFIER_INFO is what pthread_threadid_np and
        // the native tools report.
        thread_identifier_info_data_t identifier{};
        mach_msg_type_number_t identifierCount = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(threads.at(i), THREAD_IDENTIFIER_INFO,
                        reinterpret_cast<thread_info_t>(&identifier),
                        &identifierCount) == KERN_SUCCESS) {
            times.tid = static_cast<quint64>(identifier.thread_id);
        }
        result.push_back(times);
    }

#elif defined(Q_OS_LINUX)
    // Never hard-code 100: _SC_CLK_TCK is 100 on most desktop kernels but the
    // value is configurable, and a wrong divisor silently scales every
    // percentage in the dialog.
    const long ticksPerSecond = sysconf(_SC_CLK_TCK);
    if (ticksPerSecond <= 0) {
        return result;
    }
    const QDir taskDir(QStringLiteral("/proc/self/task"));
    const QStringList tids =
        taskDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    result.reserve(tids.size());
    for (const QString& tid : tids) {
        bool okTid = false;
        const quint64 numericTid = tid.toULongLong(&okTid);
        if (!okTid) {
            continue;
        }

        QFile statFile(taskDir.filePath(tid + QStringLiteral("/stat")));
        if (!statFile.open(QIODevice::ReadOnly)) {
            continue;  // the thread exited between listing and reading
        }
        quint64 ticks = 0;
        char stateChar = '\0';
        if (!parseProcStatFields(statFile.readAll(), &ticks, &stateChar)) {
            continue;
        }

        ThreadTimes times;
        times.tid = numericTid;
        times.cpuUsecs = (ticks * 1000000ull) / static_cast<quint64>(ticksPerSecond);
        times.state = SystemInfo::runStateFromProcChar(stateChar);

        QFile commFile(taskDir.filePath(tid + QStringLiteral("/comm")));
        if (commFile.open(QIODevice::ReadOnly)) {
            times.name = QString::fromUtf8(commFile.readAll()).trimmed();
        }
        result.push_back(times);
    }

#elif defined(Q_OS_WIN)
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }
    const DWORD ownPid = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            // The snapshot is machine-wide, not per process.
            if (entry.th32OwnerProcessID != ownPid) {
                continue;
            }
            const HANDLE thread = OpenThread(
                THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == nullptr) {
                continue;
            }

            FILETIME creation{};
            FILETIME exitTime{};
            FILETIME kernel{};
            FILETIME user{};
            if (GetThreadTimes(thread, &creation, &exitTime, &kernel, &user)) {
                const auto toUsecs = [](const FILETIME& value) {
                    ULARGE_INTEGER wide{};
                    wide.LowPart = value.dwLowDateTime;
                    wide.HighPart = value.dwHighDateTime;
                    return wide.QuadPart / 10ull;  // 100 ns units
                };
                ThreadTimes times;
                times.tid = entry.th32ThreadID;
                times.cpuUsecs = toUsecs(kernel) + toUsecs(user);

                // GetThreadDescription is Windows 10 1607+; the project's
                // documented Windows target is 11 (README "Windows 11"), so it
                // is always present.
                PWSTR description = nullptr;
                if (SUCCEEDED(GetThreadDescription(thread, &description))
                    && description != nullptr) {
                    times.name = QString::fromWCharArray(description);
                    LocalFree(description);
                }
                result.push_back(times);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
#endif

    return result;
}

void SystemInfo::setCurrentThreadName(const char* name)
{
    if (name == nullptr || *name == '\0') {
        return;
    }

    // The kernel-visible half, shared with the Qt-free callers.
    AetherSDR::setCurrentThreadName(name);

    // Qt's own name too, so QThread::objectName() and the kernel agree rather
    // than offering two different answers to "which thread is this".
    if (QThread* current = QThread::currentThread()) {
        current->setObjectName(QString::fromUtf8(name));
    }
}

int SystemInfo::busiestThreadIndex(const QVector<ThreadCpuSample>& samples)
{
    if (samples.isEmpty()) {
        return -1;
    }
    int busiest = 0;
    for (int i = 1; i < samples.size(); ++i) {
        // Strictly greater keeps the FIRST of equal readings, which is what
        // stops the summary line flickering between two idle threads.
        if (samples.at(i).cpuPercentOfCore > samples.at(busiest).cpuPercentOfCore) {
            busiest = i;
        }
    }
    return busiest;
}

bool SystemInfo::crossedThreshold(double previousPercent, double currentPercent,
                                  double threshold)
{
    return currentPercent > threshold && !(previousPercent > threshold);
}

ThreadRunState SystemInfo::runStateFromProcChar(char state)
{
    switch (state) {
    case 'R': return ThreadRunState::Running;
    // 'S' is an interruptible sleep and 'I' the idle variant the kernel uses
    // for threads it does not want counted toward load. Both are waiting.
    case 'S':
    case 'I': return ThreadRunState::Waiting;
    case 'D': return ThreadRunState::Uninterruptible;
    // 'T' is stopped by a signal, 't' stopped by a tracer. Same thing to read.
    case 'T':
    case 't': return ThreadRunState::Stopped;
    case 'Z': return ThreadRunState::Zombie;
    // 'X'/'x' (dead) deliberately fall through to Unknown: a dead thread is not
    // halted at a clean point, and saying Halted would be a different claim.
    default:  return ThreadRunState::Unknown;
    }
}

QVector<ThreadCpuSample> SystemInfo::cpuPercentBetween(const QVector<ThreadTimes>& previous,
                                                       const QVector<ThreadTimes>& current,
                                                       quint64 elapsedUsecs)
{
    QHash<quint64, quint64> previousByTid;
    previousByTid.reserve(previous.size());
    for (const ThreadTimes& times : previous) {
        previousByTid.insert(times.tid, times.cpuUsecs);
    }

    QVector<ThreadCpuSample> result;
    result.reserve(current.size());
    for (const ThreadTimes& times : current) {
        ThreadCpuSample sample;
        sample.tid = times.tid;
        sample.name = times.name;
        sample.cpuUsecs = times.cpuUsecs;
        // From `current`: the state is what the thread is doing now, not an
        // average over the interval.
        sample.state = times.state;

        const auto found = previousByTid.constFind(times.tid);
        if (found != previousByTid.constEnd() && elapsedUsecs > 0) {
            // Clamped, not subtracted blindly: a tid can be reused after a
            // thread exits, and a reused id whose counter restarts lower would
            // otherwise report a negative percentage.
            const quint64 before = *found;
            const quint64 delta = times.cpuUsecs > before ? times.cpuUsecs - before : 0;
            sample.cpuPercentOfCore =
                (static_cast<double>(delta) * 100.0) / static_cast<double>(elapsedUsecs);
        }
        result.push_back(sample);
    }
    return result;
}

}  // namespace AetherSDR
