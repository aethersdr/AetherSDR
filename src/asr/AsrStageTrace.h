#pragma once

#include <QDebug>
#include <QElapsedTimer>
#include <QMessageLogger>
#include <QString>

#include <atomic>
#include <source_location>

namespace AetherSDR {

// Writes whatever the log writer has queued to disk, now. aetherasr does not
// link LogManager (neither do the asr tests, which compile these sources against
// Qt6::Core alone), so the application installs the flush instead of this
// library calling it. Unset = no flush, which is what the tests want.
using AsrLogFlushHook = void (*)();

namespace detail {
inline std::atomic<AsrLogFlushHook>& asrLogFlushHookSlot()
{
    static std::atomic<AsrLogFlushHook> hook{nullptr};
    return hook;
}

inline std::atomic<int>& asrOpenStageCount()
{
    static std::atomic<int> count{0};
    return count;
}
} // namespace detail

inline void asrSetLogFlushHook(AsrLogFlushHook hook)
{
    detail::asrLogFlushHookSlot().store(hook);
}

inline void asrFlushLog()
{
    if (const AsrLogFlushHook hook = detail::asrLogFlushHookSlot().load()) {
        hook();
    }
}

// True while any AsrStageTrace scope is open, on any thread.
inline bool asrStageOpen()
{
    return detail::asrOpenStageCount().load() > 0;
}

// Begin/end record around an ASR stage that can take the process down below
// anything a caller can catch: ggml device discovery and the whisper model load
// (#5190). The begin record is flushed BEFORE the stage runs, so a support log
// from a session that died there ends by naming the stage and what it was
// attempting. Without it that log holds no ASR line at all: the writer batches
// on a 250 ms timer and flushes synchronously only on QtFatalMsg, and a SIGSEGV
// or an illegal instruction is neither.
//
// Same doctrine as core/ShutdownTrace.h — read that header for why this is an
// uncategorized QMessageLogger and deliberately absent from LogManager's
// registry: a record a user can switch off is useless in the log of a crash
// nobody can reproduce on request. It is a sibling rather than a reuse because
// a hang leaves the process alive for the writer's timer, so ShutdownTrace needs
// no flush; a crash does not. These records also carry the attempt's details,
// and must not file a model load under "aether.shutdown".
//
// One-shot by construction: discovery and load happen a handful of times per
// session. Never wrap a per-segment or per-decode path in one — each record is
// a blocking handshake with the log writer thread.
class AsrStageTrace final
{
public:
    // `attempt` is a short key=value tail (no newlines) naming what is tried.
    explicit AsrStageTrace(const char* phase, const QString& attempt = QString(),
                           std::source_location where = std::source_location::current())
        : m_phase(phase), m_where(where)
    {
        m_timer.start();
        detail::asrOpenStageCount().fetch_add(1);
        {
            QDebug out = message(m_where);
            out.noquote().nospace() << "phase=" << m_phase << " event=begin";
            if (!attempt.isEmpty()) {
                out << ' ' << attempt;
            }
        } // the record is emitted when `out` goes out of scope — before the flush
        asrFlushLog();
    }

    ~AsrStageTrace()
    {
        message(m_where).noquote().nospace()
            << "phase=" << m_phase << " event=end"
            << " result=" << (m_why ? m_why : "ok")
            << " elapsed_ms=" << m_timer.elapsed();
        asrFlushLog();
        detail::asrOpenStageCount().fetch_sub(1);
    }

    // Record that the stage did not succeed. `why` is a short stable token (no
    // spaces) so a log scraper can group on it. First call wins.
    void fail(const char* why) noexcept
    {
        if (!m_why) {
            m_why = why;
        }
    }

    AsrStageTrace(const AsrStageTrace&) = delete;
    AsrStageTrace& operator=(const AsrStageTrace&) = delete;

private:
    static QDebug message(const std::source_location& where)
    {
        return QMessageLogger(where.file_name(), static_cast<int>(where.line()),
                              where.function_name(), "aether.asr.stage").info();
    }

    const char* m_phase;
    const char* m_why = nullptr; // nullptr = ok
    std::source_location m_where;
    QElapsedTimer m_timer;
};

} // namespace AetherSDR
