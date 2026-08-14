#pragma once

#include <QDebug>
#include <QElapsedTimer>
#include <QMessageLogger>

#include <source_location>

namespace AetherSDR {

// A deliberately small shutdown breadcrumb. If teardown blocks inside a scope,
// the support log ends with its begin record; successful scopes add elapsed time
// and an explicit outcome.
//
// THE OUTCOME IS NOT OPTIONAL, because "end" alone lies about a timeout. A join
// that never completes still leaves its scope when wait(3000) gives up, so
// without fail() the record reads `event=end elapsed_ms=3000` — indistinguishable
// from a clean join to every reader except one who happens to know the timeout is
// 3000. That is exactly the case this instrumentation exists to catch on a
// Windows force-quit log, so every bounded wait must report which way it went.
//
// NOT A Q_LOGGING_CATEGORY, and deliberately absent from LogManager's registry
// table. QMessageLogger(..., "aether.shutdown").info() bypasses category
// filtering entirely — unlike the qCInfo(cat) macro it never consults the
// category's enabled state, so the string only ends up in the message context.
// These records are therefore in EVERY support log, which is the one property
// they need: a hang cannot be reproduced on request with logging turned up, and
// a breadcrumb you can accidentally switch off is useless in a force-quit log.
// Registering it would put a toggle in the Log Settings UI that either does
// nothing or, worse, works. Cf. the aether.hl2.tx note in LogManager.cpp, which
// documents the OPPOSITE failure (a category invisible because it was missing
// from that table) — the lesson there does not apply here, and someone applying
// it anyway would silently make these suppressible.
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
