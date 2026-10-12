#pragma once

#include "ByteRelay.h"

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <vector>

class QTcpServer;
class QTcpSocket;

namespace AetherSDR {

// Opaque TCP forwarder for up to Tuning::maxClients concurrent clients. Each
// accepted downstream connection gets its own fresh upstream connection;
// bytes flow through a ByteRelay unchanged in both directions, and no byte of
// one pair ever reaches another. Clients beyond the cap are closed without a
// byte. The proxy never writes anything of its own into either stream: local
// failures surface only through lastError() and the log. IPv4 only.
// Design: docs/ctr2-tcp-proxy-design.md.
class TcpByteProxy : public QObject {
    Q_OBJECT

public:
    enum class State { Stopped, Listening, Connecting, Relaying, Closing, Error };

    struct Config {
        QHostAddress listenAddress;
        quint16 listenPort{0};   // 0 = ephemeral (tests); listeningPort() reports it
        QHostAddress upstreamAddress;
        quint16 upstreamPort{0};
    };

    struct Tuning {
        // Concurrent clients; each one holds its own upstream connection.
        int maxClients{1};
        int connectTimeoutMs{10000};
        qint64 socketReadBufferBytes{256 * 1024};
        ByteRelay::Limits relay;
    };

    struct Stats {
        quint64 toUpstream{0};     // downstream -> upstream, accepted for send
        quint64 toDownstream{0};   // upstream -> downstream, accepted for send
        qint64 queuedToUpstream{0};
        qint64 queuedToDownstream{0};
        quint64 rejectedClients{0};
        // USB mode only: UDP datagrams relayed whole, and those dropped
        // because the link toward the CTR2 was backed up.
        quint64 datagramsToRadio{0};
        quint64 datagramsToDevice{0};
        quint64 datagramsDropped{0};
    };

    // One live client and its own upstream connection.
    struct Client {
        quint64 id{0};        // the connection generation it was accepted in
        QString peer;         // downstream address:port
        State state{State::Connecting};   // Connecting, Relaying or Closing
        Stats stats;
    };

    // Upstream sockets come from here so tests can substitute a socket whose
    // connect never completes. Must return a new, unconnected socket.
    using SocketFactory = std::function<QTcpSocket*()>;

    explicit TcpByteProxy(QObject* parent = nullptr);
    ~TcpByteProxy() override;

    void setTuning(const Tuning& tuning) { m_tuning = tuning; }
    void setUpstreamSocketFactory(SocketFactory factory);

    // Empty when the configuration may be started. listenPort 0 skips the
    // self-proxy port comparison, which start() repeats after binding.
    static QString validate(const Config& config);

    bool start(const Config& config);
    void stop();

    // With clients live: Relaying if any client relays, else Connecting if any
    // is connecting, else Closing. Listening with none.
    State state() const { return m_state; }
    QString lastError() const { return m_lastError; }
    QString listenerDescription() const;
    // The live clients' endpoints, comma-separated.
    QString peerDescription() const;
    QString upstreamDescription() const;
    quint16 listeningPort() const;
    // Sums the live clients. With none live it holds the last client's final
    // figures until the next client connects. rejectedClients counts since start().
    Stats stats() const;
    QList<Client> clients() const;
    quint64 connectionGeneration() const { return m_generation; }

    static QString stateName(State state);

signals:
    void stateChanged(AetherSDR::TcpByteProxy::State state);
    void statsChanged();
    void endpointsChanged();
    void lastErrorChanged(const QString& message);

private:
    class Session;
    friend class Session;

    void onNewConnection();
    void sessionConnected(quint64 generation);
    void sessionDraining(quint64 generation);
    void sessionEnded(quint64 generation, const QString& message, bool error);
    Session* findSession(quint64 generation) const;
    void teardownSession(Session* session);
    void updateState();
    void setState(State state);
    void setLastError(const QString& message);

    Config m_config;
    Tuning m_tuning;
    SocketFactory m_socketFactory;
    QTcpServer* m_server{nullptr};
    std::vector<Session*> m_sessions;   // live clients, in accept order
    quint64 m_generation{0};
    State m_state{State::Stopped};
    QString m_lastError;
    Stats m_lastSessionStats;
    quint64 m_rejectedClients{0};
    bool m_stopping{false};
};

} // namespace AetherSDR
