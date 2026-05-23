#include "MqttClient.h"
#include "LogManager.h"

#ifdef HAVE_MQTT
#include <mosquitto.h>
#endif

namespace MasterSDR {

#ifdef HAVE_MQTT
static bool s_mosquittoInitialized = false;
#endif

MqttClient::MqttClient(QObject* parent)
    : QObject(parent)
{
#ifdef HAVE_MQTT
    if (!s_mosquittoInitialized) {
        mosquitto_lib_init();
        s_mosquittoInitialized = true;
    }
#endif

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this] {
        if (!m_connected && !m_host.isEmpty()) {
            qCDebug(lcMqtt) << "MqttClient: reconnecting to" << m_host << m_port;
            connectToBroker(m_host, m_port, m_username, m_password, m_useTls, m_caFile);
        }
    });

#ifdef HAVE_MQTT
    // Windows fallback: poll mosquitto_loop() since pthreads not available
    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        if (m_mosq) { mosquitto_loop(m_mosq, 0, 1); }
    });
#endif
}

MqttClient::~MqttClient()
{
    disconnect();
#ifdef HAVE_MQTT
    if (m_mosq) {
        mosquitto_loop_stop(m_mosq, true);  // force-stop thread before destroy
        mosquitto_destroy(m_mosq);
        m_mosq = nullptr;
    }
#endif
}

void MqttClient::connectToBroker(const QString& host, quint16 port,
                                  const QString& username,
                                  const QString& password,
                                  bool useTls,
                                  const QString& caFile)
{
#ifdef HAVE_MQTT
    m_host = host;
    m_port = port;
    m_username = username;
    m_password = password;
    m_useTls   = useTls;
    m_caFile   = caFile;
    m_reconnectTimer.stop();

    if (m_mosq) {
        mosquitto_loop_stop(m_mosq, true);
        mosquitto_destroy(m_mosq);
        m_mosq = nullptr;
    }

    m_mosq = mosquitto_new(nullptr, true, this);
    if (!m_mosq) {
        qCWarning(lcMqtt) << "MqttClient: mosquitto_new failed";
        emit connectionError("Failed to create MQTT client");
        return;
    }

    mosquitto_connect_callback_set(m_mosq, onConnect);
    mosquitto_disconnect_callback_set(m_mosq, onDisconnect);
    mosquitto_message_callback_set(m_mosq, onMessage);

    if (!username.isEmpty()) {
        mosquitto_username_pw_set(m_mosq,
            username.toUtf8().constData(),
            password.isEmpty() ? nullptr : password.toUtf8().constData());
    }

#ifdef HAVE_MQTT_TLS
    if (useTls) {
        // caFile: path to CA certificate bundle, or nullptr to use the system default.
        const char* ca = caFile.isEmpty() ? nullptr : caFile.toUtf8().constData();
        int tlsRc = mosquitto_tls_set(m_mosq, ca, nullptr, nullptr, nullptr, nullptr);
        if (tlsRc != MOSQ_ERR_SUCCESS) {
            qCWarning(lcMqtt) << "MqttClient: TLS setup failed:" << mosquitto_strerror(tlsRc);
            emit connectionError(QString("TLS setup failed: %1").arg(mosquitto_strerror(tlsRc)));
            return;
        }
        qCDebug(lcMqtt) << "MqttClient: TLS enabled"
                        << (caFile.isEmpty() ? "(system CA bundle)" : caFile);
    }
#endif

    qCDebug(lcMqtt) << "MqttClient: connecting to" << host << ":" << port
                    << (useTls ? "[TLS]" : "");

    int rc = mosquitto_connect_async(m_mosq,
        host.toUtf8().constData(), port, 60);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_CONN_PENDING) {
        qCWarning(lcMqtt) << "MqttClient: connect failed:" << mosquitto_strerror(rc);
        emit connectionError(QString("Connect failed: %1").arg(mosquitto_strerror(rc)));
        int delay = qMin(kInitialReconnectMs * (1 << m_reconnectAttempts), kMaxReconnectMs);
        m_reconnectAttempts++;
        m_reconnectTimer.start(delay);
        return;
    }

#ifdef Q_OS_WIN
    // Windows: no pthreads, poll via QTimer instead of mosquitto_loop_start()
    m_pollTimer.start(50);
#else
    mosquitto_loop_start(m_mosq);
#endif
#else  // !HAVE_MQTT
    Q_UNUSED(host); Q_UNUSED(port);
    Q_UNUSED(username); Q_UNUSED(password);
    qCWarning(lcMqtt) << "MqttClient: MQTT support not compiled (install libmosquitto-dev)";
#endif
}

void MqttClient::disconnect()
{
    m_reconnectTimer.stop();
    m_pollTimer.stop();
    m_reconnectAttempts = 0;
#ifdef HAVE_MQTT
    if (m_mosq) {
        mosquitto_disconnect(m_mosq);
#ifndef Q_OS_WIN
        mosquitto_loop_stop(m_mosq, false);
#endif
    }
#endif
    m_connected = false;
}

void MqttClient::subscribe(const QString& topic)
{
#ifdef HAVE_MQTT
    if (m_connected && m_mosq) {
        int rc = mosquitto_subscribe(m_mosq, nullptr, topic.toUtf8().constData(), 0);
        if (rc != MOSQ_ERR_SUCCESS) {
            qCWarning(lcMqtt) << "MqttClient: subscribe failed for" << topic
                              << mosquitto_strerror(rc);
        } else {
            qCDebug(lcMqtt) << "MqttClient: subscribed to" << topic;
        }
    } else {
        m_pendingTopics.append(topic);
    }
#else
    Q_UNUSED(topic);
#endif
}

void MqttClient::unsubscribe(const QString& topic)
{
#ifdef HAVE_MQTT
    m_pendingTopics.removeAll(topic);
    if (m_connected && m_mosq) {
        mosquitto_unsubscribe(m_mosq, nullptr, topic.toUtf8().constData());
    }
#else
    Q_UNUSED(topic);
#endif
}

void MqttClient::publish(const QString& topic, const QByteArray& payload)
{
#ifdef HAVE_MQTT
    if (!m_connected || !m_mosq) return;
    mosquitto_publish(m_mosq, nullptr,
        topic.toUtf8().constData(),
        payload.size(), payload.constData(), 0, false);
#else
    Q_UNUSED(topic); Q_UNUSED(payload);
#endif
}

// ── Static callbacks (called from mosquitto's network thread) ────────────

#ifdef HAVE_MQTT
void MqttClient::onConnect(struct mosquitto*, void* obj, int rc)
{
    auto* self = static_cast<MqttClient*>(obj);
    QMetaObject::invokeMethod(self, [self, rc] {
        if (rc == 0) {
            self->m_connected = true;
            self->m_reconnectAttempts = 0;
            qCDebug(lcMqtt) << "MqttClient: connected to" << self->m_host;
            // Subscribe to pending topics
            for (const QString& topic : self->m_pendingTopics) {
                mosquitto_subscribe(self->m_mosq, nullptr,
                    topic.toUtf8().constData(), 0);
                qCDebug(lcMqtt) << "MqttClient: subscribed to" << topic;
            }
            emit self->connected();
        } else {
            QString err = QString("Connection refused: %1").arg(mosquitto_connack_string(rc));
            qCWarning(lcMqtt) << "MqttClient:" << err;
            emit self->connectionError(err);
        }
    });
}

void MqttClient::onDisconnect(struct mosquitto*, void* obj, int rc)
{
    auto* self = static_cast<MqttClient*>(obj);
    QMetaObject::invokeMethod(self, [self, rc] {
        bool wasConnected = self->m_connected.exchange(false);
        if (wasConnected) {
            qCDebug(lcMqtt) << "MqttClient: disconnected (rc=" << rc << ")";
            emit self->disconnected();
            if (rc != 0) {
                // Unexpected disconnect — schedule reconnect
                int delay = qMin(self->kInitialReconnectMs * (1 << self->m_reconnectAttempts),
                                 self->kMaxReconnectMs);
                self->m_reconnectAttempts++;
                self->m_reconnectTimer.start(delay);
                qCDebug(lcMqtt) << "MqttClient: reconnecting in" << delay << "ms";
            }
        }
    });
}

void MqttClient::onMessage(struct mosquitto*, void* obj,
                            const struct mosquitto_message* msg)
{
    auto* self = static_cast<MqttClient*>(obj);
    QString topic = QString::fromUtf8(msg->topic);
    QByteArray payload(static_cast<const char*>(msg->payload), msg->payloadlen);
    QMetaObject::invokeMethod(self, [self, topic, payload] {
        emit self->messageReceived(topic, payload);
    });
}
#endif

} // namespace MasterSDR
