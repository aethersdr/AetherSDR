#pragma once

#include "core/tnc/KissFraming.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace AetherSDR {

// A KISS-over-TCP TNC server. Host applications (APRS clients, terminal/packet
// programs, Dire Wolf-style tools) connect over TCP and exchange raw AX.25
// frames in KISS framing; AetherModem provides the AFSK modem underneath.
//
// - Cross-platform (Qt QTcpServer/QTcpSocket).
// - Multiple simultaneous clients; each gets its own resync-safe KISS decoder.
// - TCP keepalive plus slow-consumer and optional idle timeouts so dead or
//   stuck clients are reaped rather than leaking sockets/memory.
// - All lifecycle and per-frame activity is logged on the aether.ax25 category
//   (prefixed "KISS") so issues can be triaged as client-side vs RF-side.
class KissTncServer : public QObject {
    Q_OBJECT

public:
    explicit KissTncServer(QObject* parent = nullptr);
    ~KissTncServer() override;

    // Start listening on the given TCP port (all interfaces). Returns true on
    // success; on failure lastError() explains why and listeningChanged stays
    // false. Restarting on a new port is a stop() + start().
    bool start(quint16 port);
    void stop();

    bool isListening() const;
    quint16 port() const { return m_port; }
    int clientCount() const { return m_clients.size(); }
    QString lastError() const { return m_lastError; }

    quint64 framesToClients() const { return m_framesToClients; }
    quint64 framesFromClients() const { return m_framesFromClients; }

    // Max simultaneous clients; further connections are refused. Default 8.
    void setMaxClients(int n) { m_maxClients = n; }

public slots:
    // RX path: an AX.25 frame (address..info, no FCS) was decoded off the air;
    // fan it out to every connected client as a KISS data frame.
    void broadcastAx25Frame(const QByteArray& ax25NoFcs);

signals:
    // TX path: a client sent a KISS data frame; payload is the raw AX.25 frame
    // (no FCS) to key onto the air.
    void ax25FrameFromClient(const QByteArray& ax25NoFcs);

    // A non-data KISS command (TXDELAY, persistence, etc.) arrived.
    void kissParameterReceived(quint8 command, const QByteArray& value);

    void listeningChanged(bool listening);
    void clientCountChanged(int count);

    // Human-readable lifecycle line for the AetherModem window terminal.
    void activity(const QString& message);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void onSweepTimer();

private:
    struct Client {
        kiss::Decoder decoder;
        QElapsedTimer lastActivity;
        QString peer;
    };

    void closeClient(QTcpSocket* socket, const QString& reason);
    void emitClientCount();

    QTcpServer* m_server = nullptr;
    QHash<QTcpSocket*, Client> m_clients;
    QTimer* m_sweepTimer = nullptr;
    quint16 m_port = 8001;
    int m_maxClients = 8;
    QString m_lastError;
    quint64 m_framesToClients = 0;
    quint64 m_framesFromClients = 0;

    // Drop a client whose unsent backlog exceeds this (slow/stuck consumer).
    static constexpr qint64 kMaxWriteBacklogBytes = 256 * 1024;
    // Drop a client idle (no bytes received) longer than this. Generous because
    // a legitimate KISS client may sit quiet for long stretches; TCP keepalive
    // is the primary dead-peer detector.
    static constexpr qint64 kIdleTimeoutMs = 30 * 60 * 1000;
    static constexpr int kSweepIntervalMs = 30 * 1000;
};

} // namespace AetherSDR
