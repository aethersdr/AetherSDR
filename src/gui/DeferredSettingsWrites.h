#pragma once

#include <QCoreApplication>
#include <QTimer>
#include <QString>
#include <functional>
#include <map>
#include <utility>

namespace AetherSDR {

// Coalesce UI edits with a bounded delay. Callbacks own their scope and values,
// so teardown never reads a detached model or writes into a newly selected radio.
class DeferredSettingsWrites {
public:
    DeferredSettingsWrites()
    {
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { flush(); });
        if (QCoreApplication::instance()) {
            QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                             &m_timer, [this] { flush(); });
        }
    }
    ~DeferredSettingsWrites() { flush(); }

    void schedule(const QString& key, std::function<void()> write)
    {
        m_pending[key] = std::move(write);
        if (!m_timer.isActive()) {
            m_timer.start(250);
        }
    }
    void flush()
    {
        m_timer.stop();
        const auto pending = std::exchange(m_pending, {});
        for (const auto& [key, write] : pending) {
            write();
        }
    }

private:
    QTimer m_timer;
    std::map<QString, std::function<void()>> m_pending;
};

} // namespace AetherSDR
