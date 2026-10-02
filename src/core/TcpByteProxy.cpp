#include "TcpByteProxy.h"

#include "LogManager.h"

#include <QAbstractSocket>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace AetherSDR {

namespace {

QString endpointText(const QHostAddress& address, quint16 port)
{
    return QStringLiteral("%1:%2").arg(address.toString()).arg(port);
}

bool isUsableUnicastIpv4(const QHostAddress& address)
{
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    if (!ok) {
        return false;
    }
    return v4 != 0 && v4 != 0xFFFFFFFFu && !address.isMulticast();
}

class SocketEndpoint final : public ByteRelayEndpoint {
public:
    explicit SocketEndpoint(QTcpSocket* socket) : m_socket(socket) {}

    qint64 bytesAvailable() const override { return m_socket->bytesAvailable(); }
    QByteArray read(qint64 maxBytes) override { return m_socket->read(maxBytes); }
    qint64 write(const QByteArray& data) override
    {
        if (m_socket->state() != QAbstractSocket::ConnectedState) {
            return -1;
        }
        return m_socket->write(data);
    }
    qint64 bytesToWrite() const override { return m_socket->bytesToWrite(); }
    void closeGracefully() override { m_socket->disconnectFromHost(); }
    void abort() override { m_socket->abort(); }

private:
    QTcpSocket* m_socket;
};

QString endReasonText(ByteRelay::EndReason reason)
{
    switch (reason) {
    case ByteRelay::EndReason::PeerClosed:      return QStringLiteral("Closed by peer");
    case ByteRelay::EndReason::DrainTimeout:    return QStringLiteral("Drain timed out");
    case ByteRelay::EndReason::ProgressTimeout: return QStringLiteral("Stalled");
    case ByteRelay::EndReason::TransportError:  return QStringLiteral("Socket error");
    case ByteRelay::EndReason::Stopped:         return QStringLiteral("Stopped");
    case ByteRelay::EndReason::None:            break;
    }
    return QStringLiteral("Ended");
}

} // namespace

// One downstream/upstream pair. Owns both sockets and the relay; every
// callback is scoped to this object, and the proxy ignores a session whose
// generation is no longer current, so nothing from a finished pair can reach
// a newer one.
class TcpByteProxy::Session : public QObject {
public:
    Session(TcpByteProxy* proxy, quint64 generation, QTcpSocket* downstream,
            QTcpSocket* upstream, const Tuning& tuning)
        : QObject(proxy)
        , m_proxy(proxy)
        , m_generation(generation)
        , m_down(downstream)
        , m_up(upstream)
        , m_downEp(downstream)
        , m_upEp(upstream)
        , m_tuning(tuning)
    {
        m_down->setParent(this);
        m_up->setParent(this);
        m_up->setReadBufferSize(m_tuning.socketReadBufferBytes);

        // Until the upstream connects, downstream bytes wait in the socket's
        // bounded read buffer; an early close discards them.
        connect(m_down, &QAbstractSocket::disconnected, this, [this] {
            if (!m_relay) {
                end(QStringLiteral("CTR2 disconnected before the radio connection "
                                   "completed; %1 bytes discarded")
                        .arg(m_down->bytesAvailable()),
                    false);
            }
        });
        connect(m_down, &QAbstractSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError err) {
            if (m_relay) {
                return;
            }
            if (err == QAbstractSocket::RemoteHostClosedError) {
                return;  // disconnected() follows
            }
            end(QStringLiteral("CTR2 socket error before relay: %1").arg(m_down->errorString()),
                true);
        });

        connect(m_up, &QAbstractSocket::connected, this, [this] { onUpstreamConnected(); });
        connect(m_up, &QAbstractSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) {
            if (!m_relay) {
                end(QStringLiteral("Radio connection failed: %1").arg(m_up->errorString()),
                    true);
            }
        });

        m_connectTimer = new QTimer(this);
        m_connectTimer->setSingleShot(true);
        m_connectTimer->setInterval(std::max(1, m_tuning.connectTimeoutMs));
        connect(m_connectTimer, &QTimer::timeout, this, [this] {
            end(QStringLiteral("Radio connection timed out after %1 ms")
                    .arg(m_tuning.connectTimeoutMs),
                true);
        });
    }

    ~Session() override
    {
        for (QTcpSocket* s : {m_down, m_up}) {
            QObject::disconnect(s, nullptr, this, nullptr);
            s->abort();
        }
        delete m_relay;
        m_relay = nullptr;
    }

    void begin(const QHostAddress& address, quint16 port)
    {
        m_connectTimer->start();
        // The QString overload is the virtual one, so a test factory's socket
        // can intercept it.
        m_up->connectToHost(address.toString(), port);
    }

    void terminate()
    {
        if (m_ended) {
            return;
        }
        if (m_relay) {
            m_relay->stop();
            return;
        }
        end(QStringLiteral("Stopped before the radio connection completed; "
                           "%1 bytes discarded")
                .arg(m_down->bytesAvailable()),
            false);
    }

    Stats stats() const
    {
        Stats s;
        if (m_relay) {
            const ByteRelay::Stats r = m_relay->stats();
            s.toUpstream = r.forwarded[0];
            s.toDownstream = r.forwarded[1];
            s.queuedToUpstream = r.queued[0];
            s.queuedToDownstream = r.queued[1];
        } else if (!m_ended) {
            s.queuedToUpstream = m_down->bytesAvailable();
        }
        return s;
    }

    bool ended() const { return m_ended; }

private:
    void onUpstreamConnected()
    {
        if (m_ended || m_relay) {
            return;
        }
        m_connectTimer->stop();
        m_up->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        m_relay = new ByteRelay(&m_downEp, &m_upEp, m_tuning.relay);
        wire(m_down, ByteRelay::Side::A);
        wire(m_up, ByteRelay::Side::B);
        connect(m_relay, &ByteRelay::statsChanged, m_proxy, &TcpByteProxy::statsChanged);
        connect(m_relay, &ByteRelay::drainStarted, this, [this] {
            m_proxy->sessionDraining(m_generation);
        });
        connect(m_relay, &ByteRelay::finished, this,
                [this](ByteRelay::EndReason reason, const QString& message,
                       qint64 discardedA, qint64 discardedB) {
            const bool error = reason != ByteRelay::EndReason::PeerClosed
                && reason != ByteRelay::EndReason::Stopped;
            QString text = endReasonText(reason);
            if (reason != ByteRelay::EndReason::PeerClosed
                && reason != ByteRelay::EndReason::Stopped) {
                text += QStringLiteral(": ") + message;
            }
            text += QStringLiteral(" (discarded %1 bytes toward radio, %2 toward CTR2)")
                        .arg(discardedA).arg(discardedB);
            end(text, error);
        });

        m_proxy->sessionConnected(m_generation);
        if (!m_ended) {
            m_relay->start();
        }
    }

    void wire(QTcpSocket* socket, ByteRelay::Side side)
    {
        connect(socket, &QIODevice::readyRead, this, [this, side] {
            if (m_relay) {
                m_relay->notifyReadable(side);
            }
        });
        connect(socket, &QIODevice::bytesWritten, this, [this, side](qint64) {
            if (m_relay) {
                m_relay->notifyBytesWritten(side);
            }
        });
        connect(socket, &QIODevice::readChannelFinished, this, [this, side] {
            if (m_relay) {
                m_relay->notifyEndOfInput(side);
            }
        });
        connect(socket, &QAbstractSocket::disconnected, this, [this, side] {
            if (m_relay) {
                m_relay->notifyClosed(side);
            }
        });
        connect(socket, &QAbstractSocket::errorOccurred, this,
                [this, side, socket](QAbstractSocket::SocketError err) {
            if (!m_relay) {
                return;
            }
            if (err == QAbstractSocket::RemoteHostClosedError) {
                m_relay->notifyEndOfInput(side);
                return;
            }
            m_relay->notifyError(side, socket->errorString());
        });
    }

    void end(const QString& message, bool error)
    {
        if (m_ended) {
            return;
        }
        m_ended = true;
        m_connectTimer->stop();
        m_finalStats = stats();
        if (!m_relay) {
            m_down->abort();
            m_up->abort();
        }
        m_proxy->sessionEnded(m_generation, message, error);
    }

public:
    Stats m_finalStats;

private:
    TcpByteProxy* m_proxy;
    quint64 m_generation;
    QTcpSocket* m_down;
    QTcpSocket* m_up;
    SocketEndpoint m_downEp;
    SocketEndpoint m_upEp;
    Tuning m_tuning;
    ByteRelay* m_relay{nullptr};
    QTimer* m_connectTimer{nullptr};
    bool m_ended{false};
};

TcpByteProxy::TcpByteProxy(QObject* parent)
    : QObject(parent)
    , m_socketFactory([] { return new QTcpSocket; })
{
}

TcpByteProxy::~TcpByteProxy()
{
    if (m_session) {
        m_session->terminate();
    }
    teardownSession();
    delete m_server;
    m_server = nullptr;
}

void TcpByteProxy::setUpstreamSocketFactory(SocketFactory factory)
{
    m_socketFactory = factory ? std::move(factory)
                              : SocketFactory([] { return new QTcpSocket; });
}

QString TcpByteProxy::validate(const Config& config)
{
    if (!isUsableUnicastIpv4(config.listenAddress)) {
        return QStringLiteral("Select a specific local IPv4 listen address");
    }
    if (!isUsableUnicastIpv4(config.upstreamAddress)) {
        return QStringLiteral("Enter the radio's IPv4 address");
    }
    if (config.upstreamPort == 0) {
        return QStringLiteral("Radio port must be 1-65535");
    }
    if (config.listenPort != 0 && config.upstreamPort == config.listenPort) {
        const QHostAddress up(config.upstreamAddress.toIPv4Address());
        bool local = up.isLoopback()
            || up == QHostAddress(config.listenAddress.toIPv4Address());
        if (!local) {
            for (const QHostAddress& a : QNetworkInterface::allAddresses()) {
                bool ok = false;
                const quint32 v4 = a.toIPv4Address(&ok);
                if (ok && v4 == up.toIPv4Address()) {
                    local = true;
                    break;
                }
            }
        }
        if (local) {
            return QStringLiteral("Radio address %1 is this computer's own listener; "
                                  "the proxy would connect to itself")
                .arg(endpointText(up, config.upstreamPort));
        }
    }
    return {};
}

bool TcpByteProxy::start(const Config& config)
{
    if (m_state != State::Stopped && m_state != State::Error) {
        return false;
    }
    const QString invalid = validate(config);
    if (!invalid.isEmpty()) {
        setLastError(invalid);
        setState(State::Error);
        return false;
    }

    m_config = config;
    m_config.listenAddress = QHostAddress(config.listenAddress.toIPv4Address());
    m_config.upstreamAddress = QHostAddress(config.upstreamAddress.toIPv4Address());
    m_lastSessionStats = {};
    m_rejectedClients = 0;
    m_peerDescription.clear();

    delete m_server;
    m_server = new QTcpServer(this);
    // A busy address or port is reported as-is; there is no fallback.
    if (!m_server->listen(m_config.listenAddress, m_config.listenPort)) {
        const QString err = QStringLiteral("Cannot listen on %1: %2")
            .arg(endpointText(m_config.listenAddress, m_config.listenPort),
                 m_server->errorString());
        delete m_server;
        m_server = nullptr;
        qCWarning(lcDevices) << "CTR2 proxy:" << err;
        setLastError(err);
        setState(State::Error);
        emit endpointsChanged();
        return false;
    }

    Config bound = m_config;
    bound.listenPort = m_server->serverPort();
    const QString selfProxy = validate(bound);
    if (!selfProxy.isEmpty()) {
        delete m_server;
        m_server = nullptr;
        setLastError(selfProxy);
        setState(State::Error);
        emit endpointsChanged();
        return false;
    }

    connect(m_server, &QTcpServer::newConnection, this, &TcpByteProxy::onNewConnection);
    ++m_generation;
    setLastError({});
    qCInfo(lcDevices) << "CTR2 proxy: listening on" << listenerDescription()
                      << "-> radio" << upstreamDescription();
    setState(State::Listening);
    emit endpointsChanged();
    emit statsChanged();
    return true;
}

void TcpByteProxy::stop()
{
    m_stopping = true;
    if (m_session) {
        m_session->terminate();
    }
    m_stopping = false;
    teardownSession();
    ++m_generation;
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
        qCInfo(lcDevices) << "CTR2 proxy: stopped";
    }
    m_peerDescription.clear();
    setState(State::Stopped);
    emit endpointsChanged();
    emit statsChanged();
}

void TcpByteProxy::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        if (!socket) {
            break;
        }
        if (m_session) {
            // One active client: close the newcomer without writing anything.
            ++m_rejectedClients;
            qCInfo(lcDevices) << "CTR2 proxy: rejected additional client"
                              << endpointText(socket->peerAddress(), socket->peerPort());
            socket->close();
            socket->deleteLater();
            emit statsChanged();
            continue;
        }
        socket->setReadBufferSize(m_tuning.socketReadBufferBytes);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        ++m_generation;
        m_lastSessionStats = {};
        m_peerDescription = endpointText(QHostAddress(socket->peerAddress().toIPv4Address()),
                                         socket->peerPort());
        QTcpSocket* upstream = m_socketFactory();
        m_session = new Session(this, m_generation, socket, upstream, m_tuning);
        qCInfo(lcDevices) << "CTR2 proxy: client" << m_peerDescription
                          << "connected; opening" << upstreamDescription();
        setState(State::Connecting);
        emit endpointsChanged();
        m_session->begin(m_config.upstreamAddress, m_config.upstreamPort);
    }
}

void TcpByteProxy::sessionConnected(quint64 generation)
{
    if (generation != m_generation) {
        return;
    }
    qCInfo(lcDevices) << "CTR2 proxy: relaying" << m_peerDescription
                      << "<->" << upstreamDescription();
    setState(State::Relaying);
}

void TcpByteProxy::sessionDraining(quint64 generation)
{
    if (generation != m_generation) {
        return;
    }
    setState(State::Closing);
}

void TcpByteProxy::sessionEnded(quint64 generation, const QString& message, bool error)
{
    if (generation != m_generation || !m_session) {
        return;
    }
    m_lastSessionStats = m_session->m_finalStats;
    if (error) {
        qCWarning(lcDevices) << "CTR2 proxy:" << message;
        setLastError(message);
    } else {
        qCInfo(lcDevices) << "CTR2 proxy:" << message;
    }
    teardownSession();
    if (m_server && !m_stopping) {
        setState(State::Listening);
    }
    emit endpointsChanged();
    emit statsChanged();
}

void TcpByteProxy::teardownSession()
{
    if (!m_session) {
        return;
    }
    Session* session = m_session;
    m_session = nullptr;
    // Deferred: this may run inside one of the session's own socket signals.
    QObject::disconnect(session, nullptr, this, nullptr);
    session->deleteLater();
}

QString TcpByteProxy::listenerDescription() const
{
    if (!m_server || !m_server->isListening()) {
        return {};
    }
    return endpointText(m_server->serverAddress(), m_server->serverPort());
}

QString TcpByteProxy::upstreamDescription() const
{
    if (m_config.upstreamAddress.isNull()) {
        return {};
    }
    return endpointText(m_config.upstreamAddress, m_config.upstreamPort);
}

quint16 TcpByteProxy::listeningPort() const
{
    return m_server && m_server->isListening() ? m_server->serverPort() : 0;
}

TcpByteProxy::Stats TcpByteProxy::stats() const
{
    Stats s = (m_session && !m_session->ended()) ? m_session->stats() : m_lastSessionStats;
    s.rejectedClients = m_rejectedClients;
    return s;
}

QString TcpByteProxy::stateName(State state)
{
    switch (state) {
    case State::Stopped:    return QStringLiteral("Stopped");
    case State::Listening:  return QStringLiteral("Listening");
    case State::Connecting: return QStringLiteral("Connecting");
    case State::Relaying:   return QStringLiteral("Relaying");
    case State::Closing:    return QStringLiteral("Closing");
    case State::Error:      return QStringLiteral("Error");
    }
    return {};
}

void TcpByteProxy::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state);
}

void TcpByteProxy::setLastError(const QString& message)
{
    if (m_lastError == message) {
        return;
    }
    m_lastError = message;
    emit lastErrorChanged(message);
}

} // namespace AetherSDR
