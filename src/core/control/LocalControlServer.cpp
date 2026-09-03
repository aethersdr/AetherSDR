#include "LocalControlServer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace AetherSDR::control {

struct LocalControlServer::Client {
    QByteArray input;
    ControlSessionState session;
    QTimer handshakeTimer;
};

LocalControlServer::LocalControlServer(QObject* parent)
    : LocalControlServer(parent, Limits{})
{
}

LocalControlServer::LocalControlServer(QObject* parent, Limits limits)
    : QObject(parent), m_limits(limits)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection,
            this, [this] { acceptConnections(); });
}

LocalControlServer::~LocalControlServer()
{
    close();
}

bool LocalControlServer::listen(const QString& name)
{
    if (m_server.isListening() || m_lock || m_limits.maxClients < 1
        || m_limits.handshakeTimeoutMs < 1 || m_limits.maxQueuedOutputBytes < 1) {
        return false;
    }

    QString endpointName;
    QString lockPath;
    if (!resolveEndpoint(name, &endpointName, &lockPath)) {
        return false;
    }

    std::unique_ptr<QLockFile> lock = std::make_unique<QLockFile>(lockPath);
    lock->setStaleLockTime(0);
    if (!lock->tryLock()) {
        return false;
    }

#ifndef Q_OS_WIN
    if (QFileInfo::exists(endpointName)) {
        QLocalSocket probe;
        probe.connectToServer(endpointName);
        if (probe.waitForConnected(100)) {
            probe.disconnectFromServer();
            return false;
        }
        if (!QLocalServer::removeServer(endpointName)) {
            return false;
        }
    }
#endif

    if (!m_server.listen(endpointName)) {
        return false;
    }
    m_lock = std::move(lock);
    return true;
}

void LocalControlServer::close()
{
    m_server.close();
    QList<QLocalSocket*> sockets;
    sockets.reserve(static_cast<qsizetype>(m_clients.size()));
    for (const auto& [socket, client] : m_clients) {
        Q_UNUSED(client);
        sockets.append(socket);
    }
    for (QLocalSocket* socket : sockets) {
        socket->disconnectFromServer();
        dropClient(socket);
    }
    m_lock.reset();
}

void LocalControlServer::acceptConnections()
{
    while (m_server.hasPendingConnections()) {
        QLocalSocket* socket = m_server.nextPendingConnection();
        if (!socket) {
            continue;
        }
        if (m_clients.size() >= static_cast<std::size_t>(m_limits.maxClients)) {
            connect(socket, &QLocalSocket::disconnected,
                    socket, &QLocalSocket::deleteLater);
            const ProtocolError limit{QStringLiteral("transport.limit_exceeded"),
                                      QStringLiteral("maximum client count reached"), {}, false};
            if (send(socket, ControlProtocolCodec::errorResponse({}, limit))) {
                socket->disconnectFromServer();
            }
            continue;
        }

        std::unique_ptr<Client> ownedClient = std::make_unique<Client>();
        Client* client = ownedClient.get();
        client->handshakeTimer.setSingleShot(true);
        client->handshakeTimer.setInterval(m_limits.handshakeTimeoutMs);
        socket->setReadBufferSize(ProtocolLimits::kMaxMessageBytes + 1);
        m_clients.emplace(socket, std::move(ownedClient));

        connect(&client->handshakeTimer, &QTimer::timeout, socket, [this, socket] {
            const ProtocolError timeout{QStringLiteral("engine.timeout"),
                                        QStringLiteral("hello handshake timed out"), {}, false};
            if (!send(socket, ControlProtocolCodec::errorResponse({}, timeout))) {
                return;
            }
            socket->disconnectFromServer();
        });
        connect(socket, &QLocalSocket::readyRead,
                this, [this, socket] { readClient(socket); });
        connect(socket, &QLocalSocket::disconnected,
                this, [this, socket] {
                    // QLocalSocket::abort() may emit disconnected synchronously
                    // from send(). Defer Client destruction so neither a
                    // readyRead handler nor the handshake timer can lose the
                    // state object whose callback is still on the stack.
                    QTimer::singleShot(0, this, [this, socket] {
                        dropClient(socket);
                    });
                    socket->deleteLater();
                });
        client->handshakeTimer.start();
    }
}

void LocalControlServer::readClient(QLocalSocket* socket)
{
    const auto clientIt = m_clients.find(socket);
    if (clientIt == m_clients.end()) {
        return;
    }
    Client* client = clientIt->second.get();
    client->input.append(socket->readAll());

    while (true) {
        const qsizetype newline = client->input.indexOf('\n');
        if (newline < 0) {
            if (client->input.size() > ProtocolLimits::kMaxMessageBytes) {
                const ProtocolError limit{
                    QStringLiteral("transport.limit_exceeded"),
                    QStringLiteral("input frame exceeds maxMessageBytes"), {}, false};
                if (!send(socket, ControlProtocolCodec::errorResponse({}, limit))) {
                    return;
                }
                socket->disconnectFromServer();
            }
            return;
        }
        QByteArray frame = client->input.left(newline);
        client->input.remove(0, newline + 1);
        if (frame.endsWith('\r')) {
            frame.chop(1);
        }

        const ServiceReply reply = m_service.handle(frame, &client->session);
        if (client->session.negotiated) {
            client->handshakeTimer.stop();
        }
        if (!send(socket, reply.message)) {
            return;
        }
        if (reply.closeAfterWrite) {
            socket->disconnectFromServer();
            return;
        }
    }
}

void LocalControlServer::dropClient(QLocalSocket* socket)
{
    m_clients.erase(socket);
}

bool LocalControlServer::send(QLocalSocket* socket, const QJsonObject& message)
{
    if (!socket || socket->state() == QLocalSocket::UnconnectedState) {
        return false;
    }
    QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    if (socket->bytesToWrite() + bytes.size() > m_limits.maxQueuedOutputBytes) {
        socket->abort();
        return false;
    }
    if (socket->write(bytes) != bytes.size()) {
        socket->abort();
        return false;
    }
    return true;
}

bool LocalControlServer::resolveEndpoint(
    const QString& logicalName, QString* endpointName, QString* lockPath)
{
    static const QRegularExpression validName(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    if (!endpointName || !lockPath || !validName.match(logicalName).hasMatch()) {
        return false;
    }

#ifdef Q_OS_WIN
    QString runtimeRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#else
    QString runtimeRoot = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
#endif
    if (runtimeRoot.isEmpty()) {
        return false;
    }

    const QString runtimeDirectory = QDir(runtimeRoot).filePath(QStringLiteral("aethersdr"));
    if (!QDir().mkpath(runtimeDirectory)) {
        return false;
    }
#ifdef Q_OS_UNIX
    if (!QFile::setPermissions(runtimeDirectory,
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner)) {
        return false;
    }
#endif
    const QFileInfo directoryInfo(runtimeDirectory);
    if (!directoryInfo.isDir() || directoryInfo.isSymLink()) {
        return false;
    }
#ifdef Q_OS_UNIX
    if (directoryInfo.ownerId() != static_cast<uint>(::getuid())) {
        return false;
    }
    const QFileDevice::Permissions forbidden =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    if ((directoryInfo.permissions() & forbidden) != 0) {
        return false;
    }
#endif

    const QByteArray digest = QCryptographicHash::hash(
        logicalName.toUtf8(), QCryptographicHash::Sha256).toHex().left(24);
#ifdef Q_OS_WIN
    *endpointName = QStringLiteral("aethersdr-%1").arg(QString::fromLatin1(digest));
#else
    *endpointName = QDir(runtimeDirectory).filePath(
        QStringLiteral("control-%1.sock").arg(QString::fromLatin1(digest)));
#endif
    *lockPath = QDir(runtimeDirectory).filePath(
        QStringLiteral("control-%1.lock").arg(QString::fromLatin1(digest)));
    return true;
}

} // namespace AetherSDR::control
