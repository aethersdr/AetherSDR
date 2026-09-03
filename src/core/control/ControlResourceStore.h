#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

#include <optional>

namespace AetherSDR::control {

struct ResourceAddress {
    QString type;
    QString radioSession;
    QString id;

    [[nodiscard]] QString key() const;
    [[nodiscard]] QJsonObject toJson() const;
    bool operator==(const ResourceAddress&) const = default;
};

struct ResourceSelector {
    QString type;
    QString radioSession;
    QString id;

    [[nodiscard]] bool matches(const ResourceAddress& address) const;
};

struct ResourceSnapshot {
    ResourceAddress resource;
    quint64 revision{0};
    QJsonObject value;

    [[nodiscard]] QJsonObject toJson() const;
};

// Main-thread authoritative cache for the bounded resources exposed by the
// AetherD control protocol. Revisions advance only when the complete canonical
// value changes, so consumers never have to merge partial model state.
class ControlResourceStore final : public QObject {
    Q_OBJECT

public:
    explicit ControlResourceStore(QObject* parent = nullptr);

    bool upsert(const ResourceAddress& address, const QJsonObject& value);
    bool remove(const ResourceAddress& address);

    [[nodiscard]] std::optional<ResourceSnapshot> get(
        const ResourceAddress& address) const;
    [[nodiscard]] QList<ResourceSnapshot> snapshot(
        const QList<ResourceSelector>& selectors) const;

signals:
    void resourceChanged(const AetherSDR::control::ResourceSnapshot& snapshot);
    void resourceRemoved(const AetherSDR::control::ResourceAddress& address,
                         quint64 revision);

private:
    QMap<QString, ResourceSnapshot> m_resources;
    QMap<QString, quint64> m_lastRevisions;
};

} // namespace AetherSDR::control
