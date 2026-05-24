#include "MqttSettings.h"

#include "AppSettings.h"
#include "MqttAntennaAlias.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {

constexpr const char* kMqttHostKey = "MqttHost";
constexpr const char* kMqttPortKey = "MqttPort";
constexpr const char* kMqttUserKey = "MqttUser";
constexpr const char* kMqttTlsKey = "MqttTls";
constexpr const char* kMqttCaFileKey = "MqttCaFile";
constexpr const char* kMqttTopicsKey = "MqttTopics";
constexpr const char* kMqttButtonsKey = "MqttButtons";
constexpr const char* kMqttEnabledKey = "MqttEnabled";
constexpr const char* kKeychainService = "AetherSDR";
constexpr const char* kKeychainKey = "mqtt_password";
constexpr const char* kLegacyPasswordKey = "MqttPass";

QString trimmed(const QString& value)
{
    return value.trimmed();
}

} // namespace

QString mqttKeychainService()
{
    return QString::fromLatin1(kKeychainService);
}

QString mqttKeychainKey()
{
    return QString::fromLatin1(kKeychainKey);
}

QString legacyMqttPasswordSettingKey()
{
    return QString::fromLatin1(kLegacyPasswordKey);
}

bool mqttConnectionEnabled()
{
    return AppSettings::instance().value(kMqttEnabledKey, QStringLiteral("False")).toString()
        == QLatin1String("True");
}

void saveMqttConnectionEnabled(bool enabled)
{
    auto& settings = AppSettings::instance();
    settings.setValue(kMqttEnabledKey, enabled ? QStringLiteral("True") : QStringLiteral("False"));
    settings.save();
}

MqttConnectionConfig loadMqttConnectionConfig()
{
    auto& settings = AppSettings::instance();

    MqttConnectionConfig config;
    config.host = settings.value(kMqttHostKey, QStringLiteral("localhost")).toString().trimmed();

    bool ok = false;
    const uint port = settings.value(kMqttPortKey, QStringLiteral("1883")).toString().toUInt(&ok);
    config.port = ok && port > 0 && port <= 65535
        ? static_cast<quint16>(port)
        : static_cast<quint16>(1883);

    config.username = settings.value(kMqttUserKey, QString()).toString().trimmed();
    config.useTls = settings.value(kMqttTlsKey, QStringLiteral("False")).toString()
        == QLatin1String("True");
    config.caFile = settings.value(kMqttCaFileKey, QString()).toString().trimmed();
    return config;
}

void saveMqttConnectionConfig(const MqttConnectionConfig& config)
{
    auto& settings = AppSettings::instance();
    settings.setValue(kMqttHostKey, config.host.trimmed());
    settings.setValue(kMqttPortKey, QString::number(config.port));
    settings.setValue(kMqttUserKey, config.username.trimmed());
    settings.setValue(kMqttTlsKey, config.useTls ? QStringLiteral("True") : QStringLiteral("False"));
    settings.setValue(kMqttCaFileKey, config.caFile.trimmed());
    settings.save();
}

QVector<MqttTopicDef> parseMqttTopicConfig(const QString& value)
{
    QVector<MqttTopicDef> topics;
    const QStringList parts = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    topics.reserve(parts.size());

    for (const QString& raw : parts) {
        QString topic = raw.trimmed();
        const bool display = topic.startsWith(QLatin1Char('*'));
        if (display) {
            topic = topic.mid(1).trimmed();
        }
        if (!topic.isEmpty()) {
            topics.append({topic, display});
        }
    }

    return topics;
}

QString serializeMqttTopicConfig(const QVector<MqttTopicDef>& topics)
{
    QStringList parts;
    parts.reserve(topics.size());

    for (const MqttTopicDef& def : topics) {
        const QString topic = def.topic.trimmed();
        if (topic.isEmpty()) {
            continue;
        }
        parts.append(def.displayOnPan
                         ? QStringLiteral("*%1").arg(topic)
                         : topic);
    }

    return parts.join(QStringLiteral(", "));
}

QVector<MqttTopicDef> loadMqttTopicConfig()
{
    return parseMqttTopicConfig(
        AppSettings::instance().value(kMqttTopicsKey, QString()).toString());
}

void saveMqttTopicConfig(const QVector<MqttTopicDef>& topics)
{
    auto& settings = AppSettings::instance();
    settings.setValue(kMqttTopicsKey, serializeMqttTopicConfig(topics));
    settings.save();
}

QStringList mqttUserSubscriptionTopics(const QVector<MqttTopicDef>& topics)
{
    QStringList names;
    for (const MqttTopicDef& def : topics) {
        const QString topic = def.topic.trimmed();
        if (topic.isEmpty() || names.contains(topic)) {
            continue;
        }
        names.append(topic);
    }
    return names;
}

QStringList internalMqttSubscriptionTopics()
{
    return {
        mqttAntennaAliasTopicPrefix() + QStringLiteral("+"),
        mqttAntennaAliasBulkTopic(),
    };
}

QStringList mqttSubscriptionTopics(const QStringList& userTopics)
{
    QStringList allTopics;

    for (const QString& raw : userTopics) {
        const QString topic = raw.trimmed();
        if (!topic.isEmpty() && !allTopics.contains(topic)) {
            allTopics.append(topic);
        }
    }

    for (const QString& topic : internalMqttSubscriptionTopics()) {
        if (!allTopics.contains(topic)) {
            allTopics.append(topic);
        }
    }

    return allTopics;
}

QStringList mqttSubscriptionTopics(const QVector<MqttTopicDef>& userTopics)
{
    return mqttSubscriptionTopics(mqttUserSubscriptionTopics(userTopics));
}

QVector<MqttButtonDef> mqttButtonsFromJson(const QString& json)
{
    QVector<MqttButtonDef> buttons;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray()) {
        return buttons;
    }

    const QJsonArray arr = doc.array();
    buttons.reserve(arr.size());
    for (const QJsonValue& value : arr) {
        const QJsonObject obj = value.toObject();
        MqttButtonDef def{
            trimmed(obj.value(QStringLiteral("label")).toString()),
            trimmed(obj.value(QStringLiteral("topic")).toString()),
            obj.value(QStringLiteral("payload")).toString(),
        };
        if (!def.label.isEmpty() || !def.topic.isEmpty() || !def.payload.isEmpty()) {
            buttons.append(def);
        }
    }

    return buttons;
}

QString mqttButtonsToJson(const QVector<MqttButtonDef>& buttons)
{
    QJsonArray arr;
    for (const MqttButtonDef& def : buttons) {
        const QString label = def.label.trimmed();
        const QString topic = def.topic.trimmed();
        if (label.isEmpty() && topic.isEmpty() && def.payload.isEmpty()) {
            continue;
        }

        QJsonObject obj;
        obj.insert(QStringLiteral("label"), label);
        obj.insert(QStringLiteral("topic"), topic);
        obj.insert(QStringLiteral("payload"), def.payload);
        arr.append(obj);
    }

    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVector<MqttButtonDef> loadMqttButtonConfig()
{
    return mqttButtonsFromJson(
        AppSettings::instance().value(kMqttButtonsKey, QString()).toString());
}

void saveMqttButtonConfig(const QVector<MqttButtonDef>& buttons)
{
    auto& settings = AppSettings::instance();
    settings.setValue(kMqttButtonsKey, mqttButtonsToJson(buttons));
    settings.save();
}

} // namespace AetherSDR
