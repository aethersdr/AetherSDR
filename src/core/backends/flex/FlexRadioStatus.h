#pragma once

#include "core/backends/RadioStatusObservation.h"
#include <QMap>
#include <mutex>

namespace AetherSDR {

class FlexRadioStatusSource {
public:
    std::shared_ptr<RadioStatusLifetime> lifetime() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_lifetime;
    }
    void begin()
    {
        const std::scoped_lock lock(m_mutex);
        m_lifetime->active.store(false, std::memory_order_release);
        m_lifetime->deliveryOpen.store(false, std::memory_order_release);
        m_lifetime = std::make_shared<RadioStatusLifetime>();
    }
    void invalidate()
    {
        const auto source = lifetime();
        source->active.store(false, std::memory_order_release);
        source->deliveryOpen.store(false, std::memory_order_release);
    }
    std::optional<RadioStatusObservation> end()
    {
        const auto source = lifetime();
        if (!source->active.exchange(false, std::memory_order_acq_rel)) { return std::nullopt; }
        RadioStatusObservation observation;
        observation.kind = RadioStatusObservation::Kind::SessionEnded;
        observation.lifetime = source;
        return observation;
    }
    ~FlexRadioStatusSource() { invalidate(); }
private:
    mutable std::mutex m_mutex;
    std::shared_ptr<RadioStatusLifetime> m_lifetime{std::make_shared<RadioStatusLifetime>()};
};

std::optional<RadioStatusObservation> decodeFlexRadioStatus(
    const QString& object, const QMap<QString, QString>& fields,
    const std::shared_ptr<RadioStatusLifetime>& lifetime, quint32 localHandle = 0);

} // namespace AetherSDR
