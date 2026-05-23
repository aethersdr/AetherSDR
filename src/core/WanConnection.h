#pragma once

#include "CommandParser.h"

#include <QObject>
#include <QSslSocket>
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QHostAddress>
#include <QTimer>
#include <functional>
#include <atomic>

namespace AetherSDR {

// Manages a TLS connection to a FlexRadio over the internet (SmartLink).
// Speaks the same V/H/R/S/M protocol as RadioConnection but over TLS,
// with an initial "wan validate handle=<h>" handshake.
//
// Emits the same signals as RadioConnection so RadioModel can use either.
class WanConnection : public QObject {
    Q_OBJECT

public:
    explicit WanConnection(QObject* parent = nullptr);
    ~WanConnection() override;

    bool isConnected() const    { return m_connected; }
    bool isSocketIdle() const   { return m_socket.state() == QAbstractSocket::UnconnectedState; }
    quint32 clientHandle() const { return m_handle; }
    QHostAddress radioAddress() const { return m_socket.peerAddress(); }
    quint16 localTcpPort() const { return m_socket.localPort(); }

    // Connect via TLS to radio's public IP with WAN handle auth
    void connectToRadio(const QString& host, quint16 tlsPort,
                        const QString& wanHandle);
    void disconnectFromRadio();

    // Same command interface as RadioConnection
    using ResponseCallback = std::function<void(int resultCode, const QString& body)>;
    quint32 sendCommand(const QString& command,
                        ResponseCallback callback = nullptr);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString& message);
    void messageReceived(const ParsedMessage& msg);
    void statusReceived(const QString& object, const QMap<QString, QString>& kvs);
    void versionReceived(const QString& version);
    void pingRttMeasured(int ms);

private slots:
    void onTlsConnected();
    void onTlsDisconnected();
    void onSslErrors(const QList<QSslError>& errors);
    void onSocketError(QAbstractSocket::SocketError error);
    void onReadyRead();
    void onHeartbeat();

private:
    void processLine(const QString& line);

    QSslSocket m_socket;
    QByteArray m_readBuffer;
    QTimer     m_heartbeat;
    QString    m_wanHandle;

    bool    m_connected{false};
    bool    m_validated{false};  // wan validate sent
    quint32 m_handle{0};
    std::atomic<quint32> m_seqCounter{1};
    quint32 m_lastPingSeq{0};
    QElapsedTimer m_pingStopwatch;

    // TOFU cert pin (GHSA-wfx7-w6p8-4jr2 phase 1).  Captured on first
    // connect to a given host; subsequent connects warn on mismatch.
    QString m_host;
    QString m_expectedFingerprintHex;

    QMap<quint32, ResponseCallback> m_pendingCallbacks;
};

} // namespace AetherSDR
