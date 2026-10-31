#pragma once

#include "core/backends/SetupStatusObservation.h"
#include <QMap>
#include <mutex>

namespace AetherSDR {

class FlexSetupStatusSource {
public:
    std::shared_ptr<SetupStatusLifetime> lifetime() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_lifetime;
    }
    void begin()
    {
        const std::scoped_lock lock(m_mutex);
        m_lifetime->retire();
        retireEntities();
        m_lifetime = std::make_shared<SetupStatusLifetime>();
    }
    void retire()
    {
        const std::scoped_lock lock(m_mutex);
        m_lifetime->retire();
        retireEntities();
    }
    std::optional<SetupStatusObservation> decode(const QString& object, const QMap<QString, QString>& fields,
                                                const std::shared_ptr<SetupStatusLifetime>& source);
    ~FlexSetupStatusSource() { retire(); }
private:
    struct Entity {
        QString family;
        std::shared_ptr<SetupStatusLifetime> lifetime;
        UsbCableStatusValues base;
    };
    void retireEntities()
    {
        for (const Entity& entity : m_entities) { entity.lifetime->retire(); }
        m_entities.clear();
    }
    QMap<QString, Entity> m_entities;
    mutable std::mutex m_mutex;
    std::shared_ptr<SetupStatusLifetime> m_lifetime{std::make_shared<SetupStatusLifetime>()};
};

std::optional<SetupStatusObservation> decodeFlexSetupStatus(
    const QString& object, const QMap<QString, QString>& fields,
    const std::shared_ptr<SetupStatusLifetime>& lifetime);

} // namespace AetherSDR
