#pragma once

#include "ControlResourceStore.h"
#include "core/discovery/RadioDiscoverySource.h"

#include <QMap>
#include <QSet>

namespace AetherSDR::control {

// Bounded, read-only projection of discovery. Owns the source; never owns or
// connects a radio. All methods and source signals run on the owning thread.
class RadioCatalogue final : public QObject {
    Q_OBJECT
public:
    static constexpr qsizetype kMaxEntries = 64;
    static constexpr qsizetype kMaxTextChars = 128;

    RadioCatalogue(std::unique_ptr<RadioDiscoverySource> source,
                   ControlResourceStore* resources, QObject* parent = nullptr);
    ~RadioCatalogue() override;
    // One discovery lifecycle per instance. Repeated start/stop calls are safe;
    // stop is terminal and clears observations, including late queued updates.
    void start();
    void stop();

private:
    void upsert(const DiscoveredRadio& radio);
    void remove(const QString& family, const QString& serial);
    void publish();

    std::unique_ptr<RadioDiscoverySource> m_source;
    ControlResourceStore* m_resources;
    QMap<QString, QJsonObject> m_entries;
    // Families already warned about a malformed endpoint, so a flood of bad
    // observations cannot turn diagnosability into a log-volume attack.
    QSet<QString> m_endpointWarnings;
    bool m_started{false};
    bool m_running{false};
    bool m_limited{false};
};

} // namespace AetherSDR::control
