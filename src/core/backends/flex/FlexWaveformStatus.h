#pragma once

#include "core/backends/WaveformStatusDelta.h"

#include <QByteArray>
#include <QChar>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <optional>

namespace AetherSDR {

// A container's base64-encoded JSON environment → its flat key/values. FlexLib
// reads Dictionary<string, string>: scalars as their text, a nested value is
// an error. Undecodable or nested is treated as empty.
inline QMap<QString, QString> flexWaveformEnvironment(const QString& base64)
{
    QMap<QString, QString> environment;
    if (base64.isEmpty()) {
        return environment;
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        base64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    QJsonParseError error{};
    const QJsonDocument doc = decoded
        ? QJsonDocument::fromJson(*decoded, &error) : QJsonDocument();
    if (!decoded || error.error != QJsonParseError::NoError || !doc.isObject()) {
        // The payload may hold secrets, so only its size is logged.
        qWarning() << "Flex waveform status: undecodable container env (" << base64.size()
                   << "chars), treating it as empty";
        return environment;
    }
    const QJsonObject object = doc.object();
    // FlexLib reads Dictionary<string, string>, which takes scalars as their text
    // and fails on a nested value.
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isArray() || it.value().isObject()) {
            qWarning() << "Flex waveform status: container env is not a flat map, treating it as empty";
            return {};
        }
        environment.insert(it.key(), it.value().isString() ? it.value().toString()
                                                           : it.value().toVariant().toString());
    }
    return environment;
}

// A Flex "waveform …" status → WaveformStatusDelta; nullopt for any other
// object, a container or restart without a name, or an empty report.
// Header-only so the waveform tests run the same parse. FlexLib Radio.cs
// UpdateWaveformsInstalledList, DockerUpdateWaveformList, ParseWfpStatus and
// ParseWaveformRestarted.
inline std::optional<WaveformStatusDelta> flexWaveformStatus(const QString& object,
                                                             const QMap<QString, QString>& kvs)
{
    WaveformStatusDelta d;
    if (object == QLatin1String("waveform")) {
        // installed_list=<entries>: comma-separated, each "<name>\x7f<version>".
        d.kind = WaveformStatusDelta::Kind::InstalledList;
        const QString raw = kvs.value(QStringLiteral("installed_list"));
        if (!raw.isEmpty()) {
            for (const QString& entry : raw.split(QLatin1Char(','))) {
                if (entry.trimmed().isEmpty()) {
                    qWarning() << "Flex waveform status: empty entry in installed_list, skipping";
                    continue;
                }
                const QStringList tokens = entry.split(QChar(0x7F), Qt::SkipEmptyParts);
                if (tokens.size() < 2) {
                    qWarning() << "Flex waveform status: malformed installed_list entry:" << entry;
                    continue;
                }
                d.installed.append({tokens.first(), tokens.last()});
            }
        }
        return d;
    }
    if (object == QLatin1String("waveform container")) {
        d.kind = WaveformStatusDelta::Kind::Container;
        d.name = kvs.value(QStringLiteral("name"));
        d.version = kvs.value(QStringLiteral("version"));
        if (d.name.isEmpty()) {
            qWarning() << "Flex waveform status: container status missing name, ignoring";
            return std::nullopt;
        }
        // "removed" is a bare word. As in FlexLib, an absent env= is an empty
        // environment: every container status is a full snapshot.
        d.removed = kvs.contains(QStringLiteral("removed"));
        if (!d.removed) { d.environment = flexWaveformEnvironment(kvs.value(QStringLiteral("env"))); }
        return d;
    }
    if (object == QLatin1String("waveform wfp_status")) {
        d.kind = WaveformStatusDelta::Kind::Wfp;
        if (kvs.contains(QStringLiteral("power"))) {
            d.powered = kvs.value(QStringLiteral("power")).compare(QLatin1String("on"), Qt::CaseInsensitive) == 0;
        }
        if (kvs.contains(QStringLiteral("ready"))) {
            d.ready = kvs.value(QStringLiteral("ready")).compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
        }
        if (kvs.contains(QStringLiteral("ipaddr"))) { d.ipAddress = kvs.value(QStringLiteral("ipaddr")); }
        return d;
    }
    if (object == QLatin1String("waveform restarted")) {
        d.kind = WaveformStatusDelta::Kind::Restarted;
        d.name = kvs.value(QStringLiteral("name"));
        if (d.name.isEmpty()) { return std::nullopt; }
        // Only result=error fails (FlexLib ParseWaveformRestarted).
        d.ok = kvs.value(QStringLiteral("result")).compare(QLatin1String("error"), Qt::CaseInsensitive) != 0;
        return d;
    }
    if (object == QLatin1String("waveform status")) {
        if (kvs.isEmpty()) { return std::nullopt; }
        d.kind = WaveformStatusDelta::Kind::Report;
        d.report = kvs;
        return d;
    }
    return std::nullopt;
}

} // namespace AetherSDR
