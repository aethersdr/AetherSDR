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
QStringList mqttSubscriptionTopics(const QVector<MqttTopicDef>& userTopics);

QVector<MqttButtonDef> mqttButtonsFromJson(const QString& json);
QString mqttButtonsToJson(const QVector<MqttButtonDef>& buttons);
QVector<MqttButtonDef> loadMqttButtonConfig();
void saveMqttButtonConfig(const QVector<MqttButtonDef>& buttons);

} // namespace AetherSDR
