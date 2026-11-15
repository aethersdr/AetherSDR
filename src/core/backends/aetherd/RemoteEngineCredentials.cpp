#include "RemoteEngineCredentials.h"

#include <QHash>

namespace AetherSDR::aetherd {

namespace {
QHash<QString, RemoteEngineRecord>& cache()
{
    static QHash<QString, RemoteEngineRecord> records;
    return records;
}
} // namespace

void RemoteEngineCredentials::setSession(const RemoteEngineRecord& record)
{
    if (record.engineId.isEmpty()) { return; }
    cache().insert(record.engineId, record);
}

std::optional<RemoteEngineRecord> RemoteEngineCredentials::session(const QString& engineId)
{
    const auto it = cache().constFind(engineId);
    if (it == cache().constEnd()) { return std::nullopt; }
    return *it;
}

QList<RemoteEngineRecord> RemoteEngineCredentials::sessions()
{
    return cache().values();
}

void RemoteEngineCredentials::clearSession(const QString& engineId)
{
    cache().remove(engineId);
}

void RemoteEngineCredentials::clearAll()
{
    cache().clear();
}

} // namespace AetherSDR::aetherd
