#include "RemoteEngineCredentials.h"

#include <QHash>
#include <QRegularExpression>

namespace AetherSDR::aetherd {

namespace {
QHash<QString, RemoteEngineRecord>& cache()
{
    static QHash<QString, RemoteEngineRecord> records;
    return records;
}
} // namespace

bool RemoteEngineCredentials::validEngineId(const QString& engineId)
{
    static const QRegularExpression hex(QStringLiteral(R"(\A[a-f0-9]{64}\z)"));
    return hex.match(engineId).hasMatch();
}

QString RemoteEngineCredentials::engineIdOf(const QString& serial)
{
    return tailnetSerial(serial) ? serial.chopped(kTailnetSuffix.size()) : serial;
}

bool RemoteEngineCredentials::tailnetSerial(const QString& serial)
{
    return serial.endsWith(kTailnetSuffix);
}

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
