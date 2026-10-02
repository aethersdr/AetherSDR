#pragma once

#include <QDebug>
#include <QElapsedTimer>
#include <QMessageLogger>

#include <source_location>

namespace AetherSDR {

// Small shutdown breadcrumb: a scope that blocks leaves its begin record last
// in the log. Every bounded wait must report its outcome via fail(), or a
// timed-out wait(3000) reads like a clean end at elapsed_ms=3000.
// Deliberately not a Q_LOGGING_CATEGORY nor in LogManager's registry:
// QMessageLogger(..., "aether.shutdown").info() bypasses category filtering,
// so these records are in every support log and can't be switched off.
class ShutdownTrace final
{
public:
    // The location defaults to the CALLER, not to this header. Captured through
    // std::source_location because __FILE__/__LINE__/Q_FUNC_INFO expand where
    // they are written: with them inside message() every breadcrumb in the log
    // reported its origin as ShutdownTrace.h, in message(), rather than the
    // teardown site it describes.
    explicit ShutdownTrace(const char* phase,
                           std::source_location where = std::source_location::current())
        : m_phase(phase), m_where(where)
    {
        m_timer.start();
        message(m_where).noquote().nospace()
            << "phase=" << m_phase << " event=begin";
    }

    ~ShutdownTrace()
    {
        message(m_where).noquote().nospace()
            << "phase=" << m_phase << " event=end"
            << " result=" << (m_why ? m_why : "ok")
            << " elapsed_ms=" << m_timer.elapsed();
    }

    // Record that the scope did NOT complete cleanly. `why` is a short stable
    // token (no spaces) so a log scraper can group on it. First call wins: the
    // first thing that went wrong is the one worth chasing.
    void fail(const char* why) noexcept
    {
        if (!m_why)
            m_why = why;
    }

    static void complete(const char* phase,
                         std::source_location where = std::source_location::current())
    {
        message(where).noquote().nospace()
            << "phase=" << phase << " event=complete";
    }

    ShutdownTrace(const ShutdownTrace&) = delete;
    ShutdownTrace& operator=(const ShutdownTrace&) = delete;

private:
    static QDebug message(const std::source_location& where)
    {
        return QMessageLogger(where.file_name(), static_cast<int>(where.line()),
                              where.function_name(), "aether.shutdown").info();
    }

    const char* m_phase;
    const char* m_why = nullptr;   // nullptr = ok
    std::source_location m_where;
    QElapsedTimer m_timer;
};

} // namespace AetherSDR
