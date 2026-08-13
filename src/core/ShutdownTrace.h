#pragma once

#include <QDebug>
#include <QElapsedTimer>
#include <QMessageLogger>

namespace AetherSDR {

// A deliberately small shutdown breadcrumb. If teardown blocks inside a scope,
// the support log ends with its begin record; successful scopes add elapsed time.
class ShutdownTrace final
{
public:
    explicit ShutdownTrace(const char* phase)
        : m_phase(phase)
    {
        m_timer.start();
        message().noquote().nospace()
            << "phase=" << m_phase << " event=begin";
    }

    ~ShutdownTrace()
    {
        message().noquote().nospace()
            << "phase=" << m_phase << " event=end elapsed_ms=" << m_timer.elapsed();
    }

    static void complete(const char* phase)
    {
        message().noquote().nospace()
            << "phase=" << phase << " event=complete";
    }

    ShutdownTrace(const ShutdownTrace&) = delete;
    ShutdownTrace& operator=(const ShutdownTrace&) = delete;

private:
    static QDebug message()
    {
        return QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO,
                              "aether.shutdown").info();
    }

    const char* m_phase;
    QElapsedTimer m_timer;
};

} // namespace AetherSDR
