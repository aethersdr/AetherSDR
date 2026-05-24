#include "MqttSettings.h"

#include "AppSettings.h"
#include "MqttAntennaAlias.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {

// Single nested-JSON key holding all MQTT settings (Principle V).  Shape:
//   {
//     "host":     "localhost",
//     "port":     1883,
//     "user":     "username",
//     "tls":      false,
//     "caFile":   "/path/to/ca.pem",
//     "topics":   [{"topic": "...", "displayOnPan": true|false}, ...],
//     "buttons":  [{"label": "...", "topic": "...", "payload": "..."}, ...],
//     "enabled":  false
//   }
//
// On first save after upgrade, the eight legacy flat keys (kMqttHostKey
// through kMqttEnabledKey) are removed from AppSettings — the migration
// is one-shot.  Loading falls back to the legacy keys when the nested
// JSON key is absent, so existing users' settings carry over transparently.
//
// The password is *not* part of this object — it lives in the system
// keychain (mqttKeychainService / mqttKeychainKey) with a legacy fallback
// at kLegacyPasswordKey.
constexpr const char* kMqttSettingsKey = "MqttSettings";

// Legacy flat keys (pre-#3051 schema).  Read for one-shot migration on
// first load, removed from AppSettings on first save.  Kept here so a
// future migration sweep (or rollback scenario) can find them easily.
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

// Forward declaration — defined below; the migration in
// mqttSettingsObject() uses these to assemble the JSON from legacy keys.
QJsonArray topicsToJsonArray(const QVector<MqttTopicDef>& topics);

// Load the nested MQTT settings object.  If the new key is absent, build
// the equivalent object from the eight legacy flat keys (transparent
// upgrade — first save will then persist the new key and remove the old
// ones).  If neither is present, returns an empty object; callers should
// supply their own defaults from the empty fields.
QJsonObject mqttSettingsObject()
{
    auto& settings = AppSettings::instance();
    const QString raw = settings.value(kMqttSettingsKey, QString()).toString();
    if (!raw.isEmpty()) {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && doc.isObject())
            return doc.object();
    }

    // Migrate from legacy flat keys.  Touch the same defaults the old
    // per-field load functions used so behaviour is unchanged when no
    // key (new or old) exists.
    QJsonObject obj;
    if (settings.contains(kMqttHostKey))
        obj.insert(QStringLiteral("host"),
                   settings.value(kMqttHostKey).toString().trimmed());
    if (settings.contains(kMqttPortKey)) {
        bool ok = false;
        const uint port = settings.value(kMqttPortKey).toString().toUInt(&ok);
        if (ok && port > 0 && port <= 65535)
            obj.insert(QStringLiteral("port"), static_cast<int>(port));
    }
    if (settings.contains(kMqttUserKey))
        obj.insert(QStringLiteral("user"),
                   settings.value(kMqttUserKey).toString().trimmed());
    if (settings.contains(kMqttTlsKey))
        obj.insert(QStringLiteral("tls"),
                   settings.value(kMqttTlsKey).toString() == QLatin1String("True"));
    if (settings.contains(kMqttCaFileKey))
        obj.insert(QStringLiteral("caFile"),
                   settings.value(kMqttCaFileKey).toString().trimmed());
    if (settings.contains(kMqttTopicsKey))
        obj.insert(QStringLiteral("topics"),
                   topicsToJsonArray(parseMqttTopicConfig(
                       settings.value(kMqttTopicsKey).toString())));
    if (settings.contains(kMqttButtonsKey)) {
        const QJsonDocument doc = QJsonDocument::fromJson(
            settings.value(kMqttButtonsKey).toString().toUtf8());
        if (doc.isArray())
            obj.insert(QStringLiteral("buttons"), doc.array());
    }
    if (settings.contains(kMqttEnabledKey))
        obj.insert(QStringLiteral("enabled"),
                   settings.value(kMqttEnabledKey).toString() == QLatin1String("True"));
    return obj;
}

// Persist the nested MQTT settings object and one-shot the legacy flat
// keys out of AppSettings on the same write.  After the first call
// post-upgrade, the legacy keys are gone and subsequent saves are pure
// no-ops on them.
void saveMqttSettingsObject(const QJsonObject& obj)
{
    auto& settings = AppSettings::instance();
    settings.setValue(kMqttSettingsKey,
                      QString::fromUtf8(
                          QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    // Strip legacy keys.  Safe to call when they don't exist.
    for (const char* legacy : {
            kMqttHostKey, kMqttPortKey, kMqttUserKey, kMqttTlsKey,
            kMqttCaFileKey, kMqttTopicsKey, kMqttButtonsKey, kMqttEnabledKey,
         })
        settings.remove(legacy);
    settings.save();
}

QJsonArray topicsToJsonArray(const QVector<MqttTopicDef>& topics)
{
    QJsonArray arr;
    for (const MqttTopicDef& def : topics) {
        const QString topic = def.topic.trimmed();
        if (topic.isEmpty()) continue;
        QJsonObject entry;
        entry.insert(QStringLiteral("topic"), topic);
        entry.insert(QStringLiteral("displayOnPan"), def.displayOnPan);
        arr.append(entry);
    }
    return arr;
}

QVector<MqttTopicDef> topicsFromJsonArray(const QJsonArray& arr)
{
    QVector<MqttTopicDef> topics;
    topics.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject obj = v.toObject();
        const QString topic = obj.value(QStringLiteral("topic")).toString().trimmed();
        if (topic.isEmpty()) continue;
        topics.append({topic, obj.value(QStringLiteral("displayOnPan")).toBool(false)});
    }
    return topics;
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
    return mqttSettingsObject().value(QStringLiteral("enabled")).toBool(false);
}

void saveMqttConnectionEnabled(bool enabled)
{
    QJsonObject obj = mqttSettingsObject();
    obj.insert(QStringLiteral("enabled"), enabled);
    saveMqttSettingsObject(obj);
}

MqttConnectionConfig loadMqttConnectionConfig()
{
    const QJsonObject obj = mqttSettingsObject();

    MqttConnectionConfig config;
    config.host = obj.value(QStringLiteral("host"))
                      .toString(QStringLiteral("localhost")).trimmed();

    const int port = obj.value(QStringLiteral("port")).toInt(1883);
    config.port = (port > 0 && port <= 65535)
        ? static_cast<quint16>(port)
        : static_cast<quint16>(1883);

    config.username = obj.value(QStringLiteral("user")).toString().trimmed();
    config.useTls = obj.value(QStringLiteral("tls")).toBool(false);
    config.caFile = obj.value(QStringLiteral("caFile")).toString().trimmed();
    return config;
}

void saveMqttConnectionConfig(const MqttConnectionConfig& config)
{
    QJsonObject obj = mqttSettingsObject();
    obj.insert(QStringLiteral("host"), config.host.trimmed());
    obj.insert(QStringLiteral("port"), static_cast<int>(config.port));
    obj.insert(QStringLiteral("user"), config.username.trimmed());
    obj.insert(QStringLiteral("tls"), config.useTls);
    obj.insert(QStringLiteral("caFile"), config.caFile.trimmed());
    saveMqttSettingsObject(obj);
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
    return topicsFromJsonArray(
        mqttSettingsObject().value(QStringLiteral("topics")).toArray());
}

void saveMqttTopicConfig(const QVector<MqttTopicDef>& topics)
{
    QJsonObject obj = mqttSettingsObject();
    obj.insert(QStringLiteral("topics"), topicsToJsonArray(topics));
    saveMqttSettingsObject(obj);
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
    // Buttons sub-array uses the same {label, topic, payload} shape that
    // mqttButtonsFromJson expects from the legacy flat key, so we can
    // serialize the sub-array and reuse the helper.
    const QJsonArray arr = mqttSettingsObject()
        .value(QStringLiteral("buttons")).toArray();
    return mqttButtonsFromJson(
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void saveMqttButtonConfig(const QVector<MqttButtonDef>& buttons)
{
    QJsonObject obj = mqttSettingsObject();
    // Re-parse our own freshly-serialized JSON to produce a QJsonArray
    // we can splice into the settings object (mqttButtonsToJson returns
    // a string, not a JSON array — match the legacy storage shape so the
    // helper stays the single source of truth for button serialization).
    const QJsonDocument doc = QJsonDocument::fromJson(
        mqttButtonsToJson(buttons).toUtf8());
    obj.insert(QStringLiteral("buttons"),
               doc.isArray() ? doc.array() : QJsonArray());
    saveMqttSettingsObject(obj);
}

} // namespace AetherSDR
