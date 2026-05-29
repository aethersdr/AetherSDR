#include "core/tnc/KissTncServer.h"

#include "core/LogManager.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace AetherSDR {

KissTncServer::KissTncServer(QObject* parent)
    : QObject(parent)
{
}

KissTncServer::~KissTncServer()
{
    stop();
}

bool KissTncServer::start(quint16 port)
{
    stop();

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &KissTncServer::onNewConnection);

    if (!m_server->listen(QHostAddress::Any, port)) {
        m_lastError = m_server->errorString();
        qCWarning(lcAx25).noquote()
            << QStringLiteral("KISS TNC failed to listen on port %1: %2").arg(port).arg(m_lastError);
        emit activity(QStringLiteral("KISS TNC could not bind port %1: %2").arg(port).arg(m_lastError));
        m_server->deleteLater();
        m_server = nullptr;
        return false;
    }

    m_port = port;
    m_lastError.clear();
    m_framesToClients = 0;
    m_framesFromClients = 0;

    m_sweepTimer = new QTimer(this);
    m_sweepTimer->setInterval(kSweepIntervalMs);
    connect(m_sweepTimer, &QTimer::timeout, this, &KissTncServer::onSweepTimer);
    m_sweepTimer->start();

    qCInfo(lcAx25).noquote()
        << QStringLiteral("KISS TNC listening on TCP port %1 (all interfaces), maxClients=%2")
               .arg(port).arg(m_maxClients);
    emit activity(QStringLiteral("KISS TNC listening on TCP port %1.").arg(port));
    emit listeningChanged(true);
    return true;
}

void KissTncServer::stop()
{
    if (m_sweepTimer) {
        m_sweepTimer->stop();
        m_sweepTimer->deleteLater();
        m_sweepTimer = nullptr;
    }

    const bool wasListening = m_server != nullptr;
    const int hadClients = m_clients.size();

    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        QTcpSocket* socket = it.key();
        socket->disconnect(this); // stop our slots firing during teardown
        socket->abort();
        socket->deleteLater();
    }
    m_clients.clear();

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

    if (wasListening) {
        qCInfo(lcAx25).noquote()
            << QStringLiteral("KISS TNC stopped (closed %1 client(s)).").arg(hadClients);
        emit activity(QStringLiteral("KISS TNC stopped."));
        emit listeningChanged(false);
        emitClientCount();
    }
}

bool KissTncServer::isListening() const
{
    return m_server != nullptr && m_server->isListening();
}

void KissTncServer::onNewConnection()
{
    if (!m_server)
        return;

    while (QTcpSocket* socket = m_server->nextPendingConnection()) {
        if (m_clients.size() >= m_maxClients) {
            const QString peer = QStringLiteral("%1:%2")
                .arg(socket->peerAddress().toString()).arg(socket->peerPort());
            qCWarning(lcAx25).noquote()
                << QStringLiteral("KISS TNC refused %1: client limit (%2) reached")
                       .arg(peer).arg(m_maxClients);
            emit activity(QStringLiteral("KISS TNC refused %1 (client limit reached).").arg(peer));
            socket->abort();
            socket->deleteLater();
            continue;
        }

        // TCP keepalive lets the OS reap a peer that vanished without a FIN.
        socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1); // Nagle off: small KISS frames

        Client client;
        client.peer = QStringLiteral("%1:%2")
            .arg(socket->peerAddress().toString()).arg(socket->peerPort());
        client.lastActivity.start();
        m_clients.insert(socket, client);

        connect(socket, &QTcpSocket::readyRead, this, &KissTncServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &KissTncServer::onDisconnected);

        qCInfo(lcAx25).noquote()
            << QStringLiteral("KISS TNC client connected: %1 (now %2 client(s))")
                   .arg(client.peer).arg(m_clients.size());
        emit activity(QStringLiteral("KISS client connected: %1.").arg(client.peer));
        emitClientCount();
    }
}

void KissTncServer::onReadyRead()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;
    auto it = m_clients.find(socket);
    if (it == m_clients.end())
        return;

    it->lastActivity.restart();
    const QByteArray chunk = socket->readAll();
    const QVector<QByteArray> frames = it->decoder.feed(chunk);

    for (const QByteArray& frame : frames) {
        quint8 portNibble = 0;
        quint8 command = 0;
        QByteArray payload;
        if (!kiss::splitTypeByte(frame, portNibble, command, payload))
            continue;

        if (command == kiss::kCmdData) {
            if (payload.isEmpty())
                continue;
            ++m_framesFromClients;
            qCDebug(lcAx25).noquote()
                << QStringLiteral("KISS TX from %1: %2 AX.25 bytes (frame #%3)")
                       .arg(it->peer).arg(payload.size()).arg(m_framesFromClients);
            emit ax25FrameFromClient(payload);
        } else if (command == kiss::kCmdReturn) {
            qCDebug(lcAx25).noquote()
                << QStringLiteral("KISS exit (return) from %1 — ignored on a TCP link").arg(it->peer);
        } else {
            qCDebug(lcAx25).noquote()
                << QStringLiteral("KISS param from %1: cmd=0x%2 bytes=%3")
                       .arg(it->peer).arg(command, 2, 16, QLatin1Char('0')).arg(payload.size());
            emit kissParameterReceived(command, payload);
        }
    }
}

void KissTncServer::broadcastAx25Frame(const QByteArray& ax25NoFcs)
{
    if (ax25NoFcs.isEmpty() || m_clients.isEmpty())
        return;

    const QByteArray kissFrame = kiss::encodeDataFrame(ax25NoFcs);
    int delivered = 0;
    QVector<QTcpSocket*> slowConsumers;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        QTcpSocket* socket = it.key();
        if (socket->state() != QAbstractSocket::ConnectedState)
            continue;
        if (socket->bytesToWrite() > kMaxWriteBacklogBytes) {
            slowConsumers.append(socket);
            continue;
        }
        socket->write(kissFrame);
        ++delivered;
    }

    if (delivered > 0)
        ++m_framesToClients;

    qCDebug(lcAx25).noquote()
        << QStringLiteral("KISS RX broadcast: %1 AX.25 bytes to %2 client(s) (frame #%3)")
               .arg(ax25NoFcs.size()).arg(delivered).arg(m_framesToClients);

    for (QTcpSocket* socket : slowConsumers)
        closeClient(socket, QStringLiteral("write backlog exceeded (slow consumer)"));
}

void KissTncServer::onDisconnected()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;
    closeClient(socket, QStringLiteral("disconnected"));
}

void KissTncServer::onSweepTimer()
{
    QVector<QTcpSocket*> stale;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->lastActivity.isValid() && it->lastActivity.elapsed() > kIdleTimeoutMs)
            stale.append(it.key());
    }
    for (QTcpSocket* socket : stale)
        closeClient(socket, QStringLiteral("idle timeout"));
}

void KissTncServer::closeClient(QTcpSocket* socket, const QString& reason)
{
    auto it = m_clients.find(socket);
    if (it == m_clients.end()) {
        socket->deleteLater();
        return;
    }
    const QString peer = it->peer;
    m_clients.erase(it);

    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();

    qCInfo(lcAx25).noquote()
        << QStringLiteral("KISS TNC client %1 closed: %2 (now %3 client(s))")
               .arg(peer, reason).arg(m_clients.size());
    emit activity(QStringLiteral("KISS client %1 closed: %2.").arg(peer, reason));
    emitClientCount();
}

void KissTncServer::emitClientCount()
{
    emit clientCountChanged(m_clients.size());
}

} // namespace AetherSDR
