#include "MqttAntennaAlias.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace AetherSDR {

namespace {

QString normalizedAntennaToken(const QString& raw)
{
    const QString token = raw.trimmed().toUpper();
    if (token.isEmpty())
        return {};

    for (const QChar ch : token) {
        if (ch == QLatin1Char('_') || ch.isLetterOrNumber())
            continue;
        return {};
    }
    return token;
}

} // namespace

QString mqttAntennaAliasTopicPrefix()
{
    return QStringLiteral("aethersdr/antenna/name/");
}

QString mqttAntennaAliasBulkTopic()
{
    return QStringLiteral("aethersdr/antenna/names");
}

QVector<MqttAntennaAliasUpdate> parseMqttAntennaAliasMessage(
    const QString& topic,
    const QByteArray& payload)
{
    QVector<MqttAntennaAliasUpdate> updates;

    const QString prefix = mqttAntennaAliasTopicPrefix();
    if (topic.startsWith(prefix)) {
        const QString token = normalizedAntennaToken(topic.mid(prefix.size()));
        if (!token.isEmpty()) {
            updates.append({token, QString::fromUtf8(payload).trimmed()});
        }
        return updates;
    }

    if (topic != mqttAntennaAliasBulkTopic())
        return updates;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return updates;

    const QJsonObject obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QString token = normalizedAntennaToken(it.key());
        if (token.isEmpty())
            continue;

        const QJsonValue value = it.value();
        if (value.isNull()) {
            updates.append({token, QString()});
        } else if (value.isString()) {
            updates.append({token, value.toString().trimmed()});
        }
    }

    return updates;
}

QVector<MqttAntennaAliasUpdate> MqttAntennaAliasQueue::receive(
    const MqttAntennaAliasUpdate& update,
    bool radioConnected,
    bool radioIdentityStable)
{
    if (radioIdentityStable)
        return {update};

    if (!radioConnected)
        return {};

    m_pending.insert(update.token, update.alias);
    return {};
}

QVector<MqttAntennaAliasUpdate> MqttAntennaAliasQueue::flush(bool radioIdentityStable)
{
    QVector<MqttAntennaAliasUpdate> updates;
    if (!radioIdentityStable)
        return updates;

    updates.reserve(m_pending.size());
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
        updates.append({it.key(), it.value()});
    m_pending.clear();
    return updates;
}

void MqttAntennaAliasQueue::clear()
{
    m_pending.clear();
}

} // namespace AetherSDR
