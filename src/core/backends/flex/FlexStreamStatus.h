#pragma once

#include "core/StreamStatus.h"
#include "core/backends/StreamStatusDelta.h"

#include <QMap>
#include <QString>

#include <optional>

namespace AetherSDR {

// A Flex "stream <id> …" or "tx_audio_stream <id> …" status → the facts the
// model's stream rules read (StreamStatusDelta); nullopt for any other object
// or an id of 0. Header-only so lightweight tests can run the same parse.
inline std::optional<StreamStatusDelta> flexStreamStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    StreamStatusDelta d;
    QString prefix = QStringLiteral("stream");
    if (!object.startsWith(prefix + QLatin1Char(' '))) {
        prefix = QStringLiteral("tx_audio_stream");
        if (!object.startsWith(prefix + QLatin1Char(' '))) { return std::nullopt; }
        d.object = StreamStatusDelta::Object::TxAudioStream;
    }
    const QString rest = object.mid(prefix.size() + 1).trimmed();
    const int firstSpace = rest.indexOf(QLatin1Char(' '));
    d.streamId = parseStatusHandle(firstSpace >= 0 ? rest.left(firstSpace) : rest);
    if (d.streamId == 0) { return std::nullopt; }
    const QString action = firstSpace >= 0 ? rest.mid(firstSpace + 1).trimmed() : QString();
    d.removed = action == QLatin1String("removed") || kvs.contains(QStringLiteral("removed"))
        || kvs.value(QStringLiteral("in_use")) == QLatin1String("0");
    d.type = kvs.value(QStringLiteral("type"));
    d.ownerReported = kvs.contains(QStringLiteral("client_handle"));
    if (d.ownerReported) { d.owner = parseStatusHandle(kvs.value(QStringLiteral("client_handle"))); }
    const auto text = [&kvs](const char* key) -> std::optional<QString> {
        if (!kvs.contains(QLatin1String(key))) { return std::nullopt; }
        return kvs.value(QLatin1String(key));
    };
    const auto number = [&kvs](const char* key) -> std::optional<int> {
        if (!kvs.contains(QLatin1String(key))) { return std::nullopt; }
        return kvs.value(QLatin1String(key)).toInt();
    };
    const auto flag = [&kvs](const char* key) -> std::optional<bool> {
        if (!kvs.contains(QLatin1String(key))) { return std::nullopt; }
        return kvs.value(QLatin1String(key)) == QLatin1String("1");
    };
    if (const auto ip = text("ip")) { d.ip = ip->trimmed(); }
    d.compression = text("compression");
    d.daxChannel = number("dax_channel");
    d.daxIqChannel = number("daxiq_channel");
    d.daxIqRate = number("daxiq_rate");
    d.slice = text("slice");
    d.pan = text("pan");
    d.active = flag("active");
    d.tx = flag("tx");
    d.keys = kvs.keys();
    return d;
}

} // namespace AetherSDR
