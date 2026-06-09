#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace AetherSDR {

struct MqttConnectionConfig {
    QString host;
    quint16 port{1883};
    QString username;
    bool useTls{false};
    QString caFile;
};

struct MqttTopicDef {
    QString topic;
    bool displayOnPan{false};
};

struct MqttButtonDef {
    QString label;
    QString topic;
    QString payload;
};

QString mqttKeychainService();
QString mqttKeychainKey();
QString legacyMqttPasswordSettingKey();

bool mqttConnectionEnabled();
void saveMqttConnectionEnabled(bool enabled);

MqttConnectionConfig loadMqttConnectionConfig();
void saveMqttConnectionConfig(const MqttConnectionConfig& config);

QVector<MqttTopicDef> parseMqttTopicConfig(const QString& value);
QString serializeMqttTopicConfig(const QVector<MqttTopicDef>& topics);
QVector<MqttTopicDef> loadMqttTopicConfig();
void saveMqttTopicConfig(const QVector<MqttTopicDef>& topics);
QStringList mqttUserSubscriptionTopics(const QVector<MqttTopicDef>& topics);

QStringList internalMqttSubscriptionTopics();
QStringList mqttSubscriptionTopics(const QStringList& userTopics);

inline constexpr QLatin1String kCwDecodeTopic   {"aethersdr/cw/decode"};
inline constexpr QLatin1String kCwTransmitTopic {"aethersdr/cw/transmit"};
// Note: relay scripts that forward cw/decode into cw/transmit should filter
// on the topic namespace ("aethersdr/...") to avoid re-publishing AetherSDR's
// own output back to it and creating a feedback loop.
inline constexpr QLatin1String kRadioStateTopic {"aethersdr/radio/state"};
inline constexpr QLatin1String kAx25RxTopic     {"aethersdr/ax25/rx"};
inline constexpr QLatin1String kAx25TxTopic     {"aethersdr/ax25/tx"};

// Describes one internal MQTT topic for display in the settings dialog and
// for driving the subscription/publish lists.  Topics with gateable=false
// (antenna alias) are always active and shown grayed-out in the dialog.
struct InternalMqttTopicDef {
    QString topic;
    QString description;
    bool    gateable{true};        // false = always on, not user-disableable
    bool    defaultEnabled{false}; // true = on by default (for topics always-on before per-topic gating was added)
};

const QVector<InternalMqttTopicDef>& internalMqttSubscribeTopicDefs();
const QVector<InternalMqttTopicDef>& internalMqttPublishTopicDefs();

bool isMqttTopicEnabled(const QString& topic);
void setMqttTopicEnabled(const QString& topic, bool enabled);

QStringList internalMqttPublishTopics();
QStringList mqttSubscriptionTopics(const QVector<MqttTopicDef>& userTopics);

QVector<MqttButtonDef> mqttButtonsFromJson(const QString& json);
QString mqttButtonsToJson(const QVector<MqttButtonDef>& buttons);
QVector<MqttButtonDef> loadMqttButtonConfig();
void saveMqttButtonConfig(const QVector<MqttButtonDef>& buttons);

} // namespace AetherSDR
