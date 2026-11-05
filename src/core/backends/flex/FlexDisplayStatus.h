#pragma once

#include "core/backends/DisplayObservation.h"
#include <QMap>
#include <mutex>

namespace AetherSDR {

// Thread-safe revocation; only the transport thread opens a new lifetime.
class FlexDisplayStatusSource {
public:
    std::shared_ptr<const std::atomic_bool> epoch() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_epoch;
    }
    void retire()
    {
        const std::scoped_lock lock(m_mutex);
        m_epoch->store(false, std::memory_order_release);
    }
    void reset()
    {
        const std::scoped_lock lock(m_mutex);
        m_epoch->store(false, std::memory_order_release);
        m_epoch = std::make_shared<std::atomic_bool>(true);
    }
    ~FlexDisplayStatusSource() { retire(); }
private:
    mutable std::mutex m_mutex;
    std::shared_ptr<std::atomic_bool> m_epoch{std::make_shared<std::atomic_bool>(true)};
};

std::optional<DisplayObservation> decodeFlexDisplayStatus(
    const QString& object, const QMap<QString, QString>& fields,
    const std::shared_ptr<const std::atomic_bool>& transportEpoch);

} // namespace AetherSDR
