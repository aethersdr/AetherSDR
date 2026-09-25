#include "LocalControlServer.h"
#include "ControlInputPump.h"
#include "ControlCredentialVault.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace AetherSDR::control {
namespace {

QJsonObject serverValue(const QString& localTransport)
{
    return {{QStringLiteral("name"), QStringLiteral("aetherd")},
            {QStringLiteral("buildVersion"), QStringLiteral(AETHERSDR_VERSION)},
            {QStringLiteral("protocolVersions"), QJsonArray{1}},
            {QStringLiteral("health"), QStringLiteral("ok")},
            {QStringLiteral("localTransport"), localTransport}};
}

} // namespace

struct LocalControlServer::Client {
    Client(ControlResourceStore* resources, qint64 maxQueuedOutputBytes,
           SessionAuthorization authorization)
        // Only created for sockets accepted by the current-user endpoint.
        : session(std::make_unique<ControlSession>(
              resources, maxQueuedOutputBytes, authorization))
    {
    }

    std::unique_ptr<ControlSession> session;
    std::unique_ptr<ControlInputPump> input;
    QTimer handshakeTimer;
};

LocalControlServer::LocalControlServer(QObject* parent)
    : LocalControlServer(parent, Limits{})
{
}

LocalControlServer::LocalControlServer(QObject* parent, Limits limits,
                                     RadioConnectionTarget* connectionTarget,
                                     bool allowLocalControl)
    // Mirror bindConnectionTarget(): a target is only installed when local
    // control is granted, so an observer-only server never carries one.
    : QObject(parent), m_resources(),
      m_service(&m_resources, allowLocalControl ? connectionTarget : nullptr), m_limits(limits),
      m_localAuthorization(allowLocalControl ? SessionAuthorization::ObserverController
                                            : SessionAuthorization::Observer)
{
    m_resources.upsert(
        {QStringLiteral("server"), {}, {}},
        serverValue(QStringLiteral("idle")));
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection,
            this, [this] { acceptConnections(); });
}

LocalControlServer::~LocalControlServer()
{
    close();
}

bool LocalControlServer::bindConnectionTarget(RadioConnectionTarget* target)
{
    return thread() == QThread::currentThread() && m_clients.empty()
        && m_localAuthorization == SessionAuthorization::ObserverController
        && m_service.bindConnectionTarget(target);
}

bool LocalControlServer::bindFrequencyTarget(SliceFrequencyTarget* target)
{
    return thread() == QThread::currentThread() && m_clients.empty()
        && m_localAuthorization == SessionAuthorization::ObserverController
        && m_service.bindFrequencyTarget(target);
}

bool LocalControlServer::bindReceiveTarget(ReceiveControlTarget* target)
{
    return thread() == QThread::currentThread() && m_clients.empty()
        && m_localAuthorization == SessionAuthorization::ObserverController
        && m_service.bindReceiveTarget(target);
}

bool LocalControlServer::bindCredentials(ControlCredentials* credentials)
{
    return thread() == QThread::currentThread() && !m_serving && m_clients.empty()
        && m_service.bindCredentials(credentials);
}

bool LocalControlServer::bindTransmitTarget(TransmitControlTarget* target)
{
    return thread() == QThread::currentThread() && !m_serving && m_clients.empty()
        && m_service.bindTransmitTarget(target);
}

std::unique_ptr<QLockFile> LocalControlServer::reserveCredentialAuthority(const QString& authorityId)
{
    if (!ControlCredentialVault::validAuthorityId(authorityId)) { return {}; }
    QString unusedEndpoint;
    QString lockPath;
    if (!resolveEndpoint(QStringLiteral("credential-authority-") + authorityId, &unusedEndpoint, &lockPath)) {
        return {};
    }
    auto lock = std::make_unique<QLockFile>(lockPath);
    lock->setStaleLockTime(0);
    if (!lock->tryLock()) { return {}; }
    return lock;
}

bool LocalControlServer::listen(const QString& name, ListenMode mode)
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
    m_resources.upsert(
        {QStringLiteral("server"), {}, {}},
        serverValue(QStringLiteral("listening")));
    return mode == ListenMode::ReserveEndpoint || startServing();
}

QString LocalControlServer::clientEndpoint(const QString& logicalName)
{
    QString endpoint;
    QString lock;
    return resolveEndpoint(logicalName, &endpoint, &lock) ? endpoint : QString{};
}

bool LocalControlServer::startServing()
{
    if (thread() != QThread::currentThread() || !m_server.isListening() || m_serving) {
        return false;
    }
    m_serving = true;
    acceptConnections();
    return true;
}

void LocalControlServer::close()
{
    if (m_closing) { return; }
    m_closing = true;
    const auto restore = qScopeGuard([this] { m_closing = false; });
    m_serving = false;
    const bool wasListening = m_server.isListening();
    m_server.close();
    QList<QLocalSocket*> sockets;
    sockets.reserve(static_cast<qsizetype>(m_clients.size()));
    for (const auto& [socket, client] : m_clients) {
        Q_UNUSED(client);
        sockets.append(socket);
    }
    for (QLocalSocket* socket : sockets) {
        const auto client = m_clients.find(socket);
        if (client != m_clients.end()) {
            client->second->input->finish();
        }
        socket->disconnectFromServer();
        dropClient(socket);
    }
    m_lock.reset();
    if (wasListening) {
        m_resources.upsert(
            {QStringLiteral("server"), {}, {}},
            serverValue(QStringLiteral("stopped")));
    }
}

void LocalControlServer::acceptConnections()
{
    while (m_server.hasPendingConnections()) {
        QLocalSocket* socket = m_server.nextPendingConnection();
        if (!socket) {
            continue;
        }
        if (!m_serving) {
            // Settings/model construction can pump a nested event loop. Do
            // not create a session or dispatch into partially initialized
            // targets. The endpoint remains claimed; clients may retry.
            socket->abort();
            socket->deleteLater();
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

        std::unique_ptr<Client> ownedClient =
            std::make_unique<Client>(&m_resources, m_limits.maxQueuedOutputBytes,
                                     m_localAuthorization);
        Client* client = ownedClient.get();
        client->handshakeTimer.setSingleShot(true);
        client->handshakeTimer.setInterval(m_limits.handshakeTimeoutMs);
        socket->setReadBufferSize(ProtocolLimits::kMaxMessageBytes + 1);
        m_clients.emplace(socket, std::move(ownedClient));
        client->input = std::make_unique<ControlInputPump>(m_service, *client->session,
            [socket](qint64 maximum) { return socket->read(maximum); },
            [this, socket](const QJsonObject& message) {
                const auto current = m_clients.find(socket);
                if (current == m_clients.end()) { return false; }
                if (current->second->session->isNegotiated()) {
                    current->second->handshakeTimer.stop();
                }
                return send(socket, message);
            },
            [socket](bool abort) {
                if (abort) { socket->abort(); }
                else { socket->disconnectFromServer(); }
            });

        connect(&client->handshakeTimer, &QTimer::timeout, socket, [this, socket] {
            const auto current = m_clients.find(socket);
            if (current == m_clients.end()) { return; }
            current->second->input->finish();
            const ProtocolError timeout{QStringLiteral("engine.timeout"),
                                        QStringLiteral("hello handshake timed out"), {}, false};
            if (!send(socket, ControlProtocolCodec::errorResponse({}, timeout))) {
                return;
            }
            socket->disconnectFromServer();
        });
        connect(socket, &QLocalSocket::readyRead,
                this, [this, socket] { readClient(socket); });
        client->session->bindOutputTransport(socket,
            [this, socket](const QByteArray& frame) { return sendFrame(socket, frame); },
            [socket] { socket->abort(); });
        connect(socket, &QLocalSocket::disconnected,
                this, [this, socket] {
                    const auto current = m_clients.find(socket);
                    if (current != m_clients.end()) {
                        current->second->handshakeTimer.stop();
                        current->second->input->finish();
                    }
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
        // Data can precede our readyRead connection when acceptance was
        // delayed by startup or another event callback.
        if (socket->bytesAvailable() > 0) {
            readClient(socket);
        }
    }
}

void LocalControlServer::readClient(QLocalSocket* socket)
{
    if (!m_serving) { return; }
    const auto clientIt = m_clients.find(socket);
    if (clientIt == m_clients.end()) {
        return;
    }
    clientIt->second->input->readAvailable();
}

void LocalControlServer::dropClient(QLocalSocket* socket)
{
    const auto current = m_clients.find(socket);
    if (current == m_clients.end()) { return; }
    // Remove from the registry before destruction can call engine cleanup.
    const std::unique_ptr<Client> retired = std::move(current->second);
    m_clients.erase(current);
}

bool LocalControlServer::send(QLocalSocket* socket, const QJsonObject& message)
{
    QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    return sendFrame(socket, bytes);
}

bool LocalControlServer::sendFrame(QLocalSocket* socket, const QByteArray& frame)
{
    if (!socket || socket->state() == QLocalSocket::UnconnectedState) {
        const auto current = m_clients.find(socket);
        if (current != m_clients.end()) { current->second->input->finish(); }
        return false;
    }
    if (socket->bytesToWrite() + frame.size() > m_limits.maxQueuedOutputBytes) {
        const auto current = m_clients.find(socket);
        if (current != m_clients.end()) { current->second->input->finish(); }
        socket->abort();
        return false;
    }
    if (socket->write(frame) != frame.size()) {
        const auto current = m_clients.find(socket);
        if (current != m_clients.end()) { current->second->input->finish(); }
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
