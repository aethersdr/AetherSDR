#include "ControlResourceStore.h"

namespace AetherSDR::control {

QString ResourceAddress::key() const
{
    return type + QChar(0x1f) + radioSession + QChar(0x1f) + id;
}

QJsonObject ResourceAddress::toJson() const
{
    QJsonObject object{{QStringLiteral("type"), type}};
    if (!radioSession.isEmpty()) {
        object.insert(QStringLiteral("radioSession"), radioSession);
    }
    if (!id.isEmpty()) {
        object.insert(QStringLiteral("id"), id);
    }
    return object;
}

bool ResourceSelector::matches(const ResourceAddress& address) const
{
    return type == address.type
        && (radioSession.isEmpty() || radioSession == address.radioSession)
        && (id.isEmpty() || id == address.id);
}

QJsonObject ResourceSnapshot::toJson() const
{
    return {{QStringLiteral("resource"), resource.toJson()},
            {QStringLiteral("revision"), static_cast<qint64>(revision)},
            {QStringLiteral("value"), value}};
}

ControlResourceStore::ControlResourceStore(QObject* parent)
    : QObject(parent)
{
}

bool ControlResourceStore::upsert(
    const ResourceAddress& address, const QJsonObject& value)
{
    const QString resourceKey = address.key();
    const auto current = m_resources.constFind(resourceKey);
    if (current != m_resources.constEnd() && current->value == value) {
        return false;
    }

    const quint64 revision = ++m_lastRevision;
    const ResourceSnapshot next{address, revision, value};
    m_resources.insert(resourceKey, next);
    emit resourceChanged(next);
    return true;
}

bool ControlResourceStore::remove(const ResourceAddress& address)
{
    const QString resourceKey = address.key();
    if (m_resources.remove(resourceKey) == 0) {
        return false;
    }
    const quint64 revision = ++m_lastRevision;
    emit resourceRemoved(address, revision);
    return true;
}

std::optional<ResourceSnapshot> ControlResourceStore::get(
    const ResourceAddress& address) const
{
    const auto found = m_resources.constFind(address.key());
    if (found == m_resources.constEnd()) {
        return std::nullopt;
    }
    return *found;
}

QList<ResourceSnapshot> ControlResourceStore::snapshot(
    const QList<ResourceSelector>& selectors) const
{
    QList<ResourceSnapshot> result;
    for (auto it = m_resources.constBegin(); it != m_resources.constEnd(); ++it) {
        for (const ResourceSelector& selector : selectors) {
            if (selector.matches(it->resource)) {
                result.append(*it);
                break;
            }
        }
    }
    return result;
}

} // namespace AetherSDR::control
