#pragma once

#include <QDeadlineTimer>
#include <atomic>
#include <memory>

namespace AetherSDR {

// Copies may cross transport threads; retirement never reads a model there.
class SessionCommandGuard {
public:
    static SessionCommandGuard create()
    {
        SessionCommandGuard guard;
        guard.m_session = std::make_shared<std::atomic<bool>>(true);
        guard.m_operation = std::make_shared<std::atomic<bool>>(true);
        return guard;
    }
    SessionCommandGuard request(qint64 timeoutMs = 30000) const
    {
        SessionCommandGuard guard;
        guard.m_session = m_session;
        guard.m_operation = std::make_shared<std::atomic<bool>>(true);
        guard.m_deadline = QDeadlineTimer(timeoutMs);
        return guard;
    }
    bool current() const
    {
        return m_session && m_session->load(std::memory_order_acquire)
            && m_operation && m_operation->load(std::memory_order_acquire)
            && !m_deadline.hasExpired();
    }
    void cancel() const
    {
        if (m_operation) { m_operation->store(false, std::memory_order_release); }
    }
    void retireSession() const
    {
        if (m_session) { m_session->store(false, std::memory_order_release); }
    }
private:
    std::shared_ptr<std::atomic<bool>> m_session;
    std::shared_ptr<std::atomic<bool>> m_operation;
    QDeadlineTimer m_deadline{QDeadlineTimer::Forever};
};

} // namespace AetherSDR
