#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>

namespace AetherSDR {

struct MqttAntennaAliasUpdate {
    QString token;
    QString alias;
};

QString mqttAntennaAliasTopicPrefix();
QString mqttAntennaAliasBulkTopic();
QVector<MqttAntennaAliasUpdate> parseMqttAntennaAliasMessage(
    const QString& topic,
    const QByteArray& payload);

class MqttAntennaAliasQueue {
public:
    QVector<MqttAntennaAliasUpdate> receive(const MqttAntennaAliasUpdate& update,
                                            bool radioConnected,
                                            bool radioIdentityStable);
    QVector<MqttAntennaAliasUpdate> flush(bool radioIdentityStable);
    void clear();
    int pendingCount() const { return m_pending.size(); }

private:
    QMap<QString, QString> m_pending;
};

} // namespace AetherSDR
