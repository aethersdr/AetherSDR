#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QTimer>
#include <QMap>
#include <atomic>

#ifdef HAVE_MQTT
struct mosquitto;
struct mosquitto_message;
#endif

namespace MasterSDR {

// Qt wrapper around libmosquitto for MQTT subscribe/publish (#699).
// Uses mosquitto_loop_start() for threaded I/O; callbacks marshal
// to the Qt main thread via QMetaObject::invokeMethod.
class MqttClient : public QObject {
    Q_OBJECT

public:
    explicit MqttClient(QObject* parent = nullptr);
    ~MqttClient() override;

    void connectToBroker(const QString& host, quint16 port,
                         const QString& username = {},
                         const QString& password = {},
                         bool useTls = false,
                         const QString& caFile = {});
    void disconnect();
    void subscribe(const QString& topic);
    void unsubscribe(const QString& topic);
    void publish(const QString& topic, const QByteArray& payload);
    bool isConnected() const { return m_connected.load(); }

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& error);
    void messageReceived(const QString& topic, const QByteArray& payload);

private:
#ifdef HAVE_MQTT
    static void onConnect(struct mosquitto* mosq, void* obj, int rc);
    static void onDisconnect(struct mosquitto* mosq, void* obj, int rc);
    static void onMessage(struct mosquitto* mosq, void* obj,
                          const struct mosquitto_message* msg);

    mosquitto* m_mosq{nullptr};
    QStringList m_pendingTopics;  // topics to subscribe after connect
#endif
    QTimer m_reconnectTimer;
    QTimer m_pollTimer;       // Windows fallback: poll mosquitto_loop()
    QString m_host;
    quint16 m_port{1883};
    QString m_username;
    QString m_password;
    bool    m_useTls{false};
    QString m_caFile;
    std::atomic<bool> m_connected{false};
    int m_reconnectAttempts{0};

    static constexpr int kInitialReconnectMs = 5000;
    static constexpr int kMaxReconnectMs = 60000;
};

} // namespace MasterSDR
