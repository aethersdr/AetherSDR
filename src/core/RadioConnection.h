#pragma once

#include "CommandParser.h"
#include "RadioDiscovery.h"

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QString>
#include <QHostAddress>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include <atomic>

namespace AetherSDR {

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error
};

// TCP connection to a FlexRadio. Designed to live on a worker thread (#502).
// Call init() after moveToThread() to create the socket and timer.
class RadioConnection : public QObject {
    Q_OBJECT

public:
    explicit RadioConnection(QObject* parent = nullptr);
    ~RadioConnection() override;

    ConnectionState state() const       { return m_state.load(); }
    quint32 clientHandle() const        { return m_handle; }
    bool isConnected() const            { return m_state.load() == ConnectionState::Connected; }
    QHostAddress radioAddress() const   { return m_radioAddr; }
    QHostAddress localAddress() const   { return m_localAddr; }
    quint16      localTcpPort() const   { return m_localPort; }
    RadioBindMode bindMode() const      { return m_bindMode; }
    QHostAddress explicitLocalBindAddress() const { return m_explicitBindAddr; }
    QHostAddress sessionLocalBindAddress() const  { return m_sessionBindAddr; }

    using ResponseCallback = std::function<void(int resultCode, const QString& body)>;

public slots:
    void init();  // Create socket + timer on the worker thread
    void connectToRadio(const RadioInfo& info);
    void connectToHost(const QHostAddress& address,
                       quint16 port = 4992,
                       RadioBindMode bindMode = RadioBindMode::Auto,
                       const QHostAddress& explicitBindAddr = {},
                       const QHostAddress& sessionBindAddr = {});
    void disconnectFromRadio();
    // Send sequenced teardown commands, wait for radio responses, then close.
    void gracefulDisconnect(quint32 handle,
                            const QString& rxStreamId,
                            quint32 streamRemoveSeq);
    // Write a pre-sequenced command to the socket. Called from RadioModel
    // via QMetaObject::invokeMethod (auto-queued to worker thread). (#502)
    void writeCommand(quint32 seq, const QString& command);

signals:
    void stateChanged(ConnectionState state);
    void connected();
    void disconnected();
    void errorOccurred(const QString& message);
    void messageReceived(const ParsedMessage& msg);
    void pingRttMeasured(int ms);
    void statusReceived(const QString& object, const QMap<QString, QString>& kvs);
    void versionReceived(const QString& version);
    // Emitted when a response (R-line) is received from the radio.
    // Callers register callbacks keyed by seq in their own maps. (#502)
    void commandResponse(quint32 seq, int resultCode, const QString& body);

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketError(QAbstractSocket::SocketError error);
    void onReadyRead();
    void onHeartbeat();

private:
    void processLine(const QString& line);
    void setState(ConnectionState s);
    bool sendCommandAndWait(quint32 seq, const QString& command, int timeoutMs);
    void writeDisconnectMarker();
    int  kernelRttMs() const;   // read smoothed RTT from kernel TCP_INFO

    QTcpSocket*  m_socket{nullptr};
    QByteArray   m_readBuffer;
    QTimer*      m_heartbeat{nullptr};

    std::atomic<ConnectionState> m_state{ConnectionState::Disconnected};
    std::atomic<quint32> m_handle{0};
    quint32 m_lastPingSeq{0};
    QElapsedTimer m_pingStopwatch;  // fallback when kernel TCP_INFO unavailable

    QHostAddress m_radioAddr;   // cached for cross-thread reads
    QHostAddress m_localAddr;
    quint16      m_localPort{0};
    RadioBindMode m_bindMode{RadioBindMode::Auto};
    QHostAddress m_explicitBindAddr;
    QHostAddress m_sessionBindAddr;

    // Callbacks removed — responses emitted via commandResponse signal (#502)
};

} // namespace AetherSDR
