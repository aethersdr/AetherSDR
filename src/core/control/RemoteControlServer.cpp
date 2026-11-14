#include "RemoteControlServer.h"
#include "ControlInputPump.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QScopeGuard>
#include <QSslCertificate>
#include <QSslKey>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

namespace AetherSDR::control {

struct RemoteControlServer::Client {
    Client(ControlResourceStore* resources, qint64 maxQueuedOutputBytes)
        : session(std::make_unique<ControlSession>(
              resources, maxQueuedOutputBytes, SessionAuthorization::NetworkScoped))
    {
    }

    std::unique_ptr<ControlSession> session;
    std::unique_ptr<ControlInputPump> input;
    // Request lines waiting for the pump: each accepted text frame plus '\n',
    // so the pump frames them exactly as it frames the local socket.
    QByteArray pending;
    QTimer handshakeTimer;
    bool handshakeExtended{false};
};

RemoteControlServer::RemoteControlServer(ControlEngine& engine, Mode mode, QObject* parent)
    : RemoteControlServer(engine, mode, parent, Limits{})
{
}

RemoteControlServer::RemoteControlServer(ControlEngine& engine, Mode mode, QObject* parent,
                                         Limits limits)
    : QObject(parent), m_engine(engine), m_mode(mode), m_limits(limits),
      m_server(std::make_unique<QWebSocketServer>(
          QStringLiteral("aetherd"),
          mode == Mode::Secure ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode))
{
    m_server->setMaxPendingConnections(m_limits.maxClients);
    m_server->setHandshakeTimeout(m_limits.handshakeTimeoutMs);
    connect(m_server.get(), &QWebSocketServer::newConnection, this, [this] { acceptConnections(); });
}

RemoteControlServer::~RemoteControlServer()
{
    close();
}

std::optional<RemoteControlServer::Endpoint> RemoteControlServer::parseEndpoint(const QString& text)
{
    QString host;
    QString port;
    if (text.startsWith(u'[')) {
        const qsizetype end = text.indexOf(QStringLiteral("]:"));
        if (end < 0) { return std::nullopt; }
        host = text.mid(1, end - 1);
        port = text.mid(end + 2);
        if (!host.contains(u':')) { return std::nullopt; }   // brackets are for IPv6
    } else {
        const qsizetype colon = text.lastIndexOf(u':');
        if (colon <= 0 || text.count(u':') != 1) { return std::nullopt; }
        host = text.left(colon);
        port = text.mid(colon + 1);
    }
    bool ok = false;
    const uint number = port.toUInt(&ok);
    QHostAddress address;
    if (!ok || number == 0 || number > 65535 || port.startsWith(u'+') || !address.setAddress(host)) {
        return std::nullopt;
    }
    // A scope id names an interface; a remote client cannot rely on it.
    if (!address.scopeId().isEmpty()) { return std::nullopt; }
    return Endpoint{address, quint16(number)};
}

QString RemoteControlServer::wireGuardInterfaceFor(const QHostAddress& address)
{
    for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
        const QString name = interface.name();
        if (!(name.startsWith(QLatin1String("tailscale")) || name.startsWith(QLatin1String("wg")))
            || !interface.flags().testFlag(QNetworkInterface::IsUp)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : interface.addressEntries()) {
            if (entry.ip().isEqual(address, QHostAddress::StrictConversion)) {
                return name;
            }
        }
    }
    return {};
}

std::optional<QSslConfiguration> RemoteControlServer::loadTlsIdentity(
    const QString& certificatePath, const QString& keyPath, QString* error)
{
    const auto fail = [error](const QString& why) -> std::optional<QSslConfiguration> {
        if (error) { *error = why; }
        return std::nullopt;
    };
    QFile certificateFile(certificatePath);
    if (!certificateFile.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("cannot read the TLS certificate '%1'").arg(certificatePath));
    }
    const QList<QSslCertificate> chain = QSslCertificate::fromData(certificateFile.readAll(), QSsl::Pem);
    if (chain.isEmpty() || chain.first().isNull()) {
        return fail(QStringLiteral("'%1' holds no PEM certificate").arg(certificatePath));
    }
    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("cannot read the TLS key '%1'").arg(keyPath));
    }
    const QByteArray keyBytes = keyFile.readAll();
    QSslKey key(keyBytes, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    if (key.isNull()) { key = QSslKey(keyBytes, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey); }
    if (key.isNull()) {
        return fail(QStringLiteral("'%1' holds no PEM EC or RSA private key").arg(keyPath));
    }
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setLocalCertificateChain(chain);
    configuration.setPrivateKey(key);
    configuration.setProtocol(QSsl::TlsV1_2OrLater);
    // Clients are authenticated by their bearer credential, not a certificate.
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    return configuration;
}

QString RemoteControlServer::certificateSha256(const QSslConfiguration& configuration)
{
    const QSslCertificate certificate = configuration.localCertificate();
    if (certificate.isNull()) { return {}; }
    return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex());
}

bool RemoteControlServer::setTlsConfiguration(const QSslConfiguration& configuration)
{
    if (m_mode != Mode::Secure || m_server->isListening()
        || configuration.localCertificate().isNull() || configuration.privateKey().isNull()) {
        return false;
    }
    m_server->setSslConfiguration(configuration);
    m_tlsReady = true;
    return true;
}

bool RemoteControlServer::listen(const Endpoint& endpoint, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error) { *error = why; }
        return false;
    };
    if (thread() != QThread::currentThread() || m_server->isListening()) {
        return fail(QStringLiteral("listener already started"));
    }
    if (endpoint.address.isNull() || endpoint.port == 0) {
        return fail(QStringLiteral("an address and a port are required"));
    }
    if (endpoint.address == QHostAddress::AnyIPv4 || endpoint.address == QHostAddress::AnyIPv6
        || endpoint.address == QHostAddress::Any) {
        return fail(QStringLiteral("a wildcard address is refused; name the one address to serve"));
    }
    if (m_mode == Mode::Secure && !m_tlsReady) {
        return fail(QStringLiteral("a TLS certificate and key are required"));
    }
    if (m_mode == Mode::Tailnet && wireGuardInterfaceFor(endpoint.address).isEmpty()) {
        return fail(QStringLiteral("%1 is not an address of an up WireGuard (tailscale*/wg*) interface")
                        .arg(endpoint.address.toString()));
    }
    if (!m_server->listen(endpoint.address, endpoint.port)) {
        return fail(m_server->errorString());
    }
    return true;
}

bool RemoteControlServer::startServing()
{
    if (thread() != QThread::currentThread() || !m_server->isListening()) { return false; }
    m_serving = true;
    return true;
}

bool RemoteControlServer::isListening() const
{
    return m_server->isListening();
}

quint16 RemoteControlServer::serverPort() const
{
    return m_server->serverPort();
}

void RemoteControlServer::close()
{
    if (m_closing) { return; }
    m_closing = true;
    const auto restore = qScopeGuard([this] { m_closing = false; });
    m_serving = false;
    m_server->close();
    QList<QWebSocket*> sockets;
    for (const auto& [socket, client] : m_clients) {
        Q_UNUSED(client);
        sockets.append(socket);
    }
    for (QWebSocket* socket : sockets) {
        const auto client = m_clients.find(socket);
        if (client != m_clients.end()) { client->second->input->finish(); }
        socket->abort();
        dropClient(socket);
        socket->deleteLater();
    }
}

void RemoteControlServer::refuse(QWebSocket* socket, const QString& code, const QString& message)
{
    const QByteArray frame = QJsonDocument(ControlProtocolCodec::errorResponse(
        {}, {code, message, {}, false})).toJson(QJsonDocument::Compact);
    socket->sendTextMessage(QString::fromUtf8(frame));
    socket->close(QWebSocketProtocol::CloseCodePolicyViolated);
}

void RemoteControlServer::acceptConnections()
{
    while (m_server->hasPendingConnections()) {
        QWebSocket* socket = m_server->nextPendingConnection();
        if (!socket) { continue; }
        connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
        if (!m_serving) {
            socket->abort();   // the engine is still starting; clients may retry
            continue;
        }
        if (m_clients.size() >= std::size_t(m_limits.maxClients)) {
            refuse(socket, QStringLiteral("transport.limit_exceeded"),
                   QStringLiteral("maximum network client count reached"));
            continue;
        }
        socket->setMaxAllowedIncomingFrameSize(ProtocolLimits::kMaxMessageBytes);
        socket->setMaxAllowedIncomingMessageSize(ProtocolLimits::kMaxMessageBytes);

        auto owned = std::make_unique<Client>(&m_engine.resources, m_limits.maxQueuedOutputBytes);
        Client* client = owned.get();
        if (m_mode == Mode::Secure) {
            client->session->allowPairing();   // the code's check digits pin this certificate
        }
        m_clients.emplace(socket, std::move(owned));
        client->input = std::make_unique<ControlInputPump>(m_engine.service, *client->session,
            [this, socket](qint64 maximum) {
                const auto current = m_clients.find(socket);
                if (current == m_clients.end()) { return QByteArray(); }
                QByteArray& pending = current->second->pending;
                const QByteArray out = pending.left(maximum);
                pending.remove(0, out.size());
                return out;
            },
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
                else { socket->close(); }
            });
        client->handshakeTimer.setSingleShot(true);
        client->handshakeTimer.setInterval(m_limits.handshakeTimeoutMs);
        connect(&client->handshakeTimer, &QTimer::timeout, socket,
                [this, socket] { handshakeExpired(socket); });
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString& message) { receiveText(socket, message); });
        connect(socket, &QWebSocket::binaryMessageReceived, this, [this, socket](const QByteArray&) {
            const auto current = m_clients.find(socket);
            if (current == m_clients.end()) { return; }
            current->second->input->finish();
            socket->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported,
                          QStringLiteral("text frames only"));
        });
        client->session->bindOutputTransport(socket,
            [this, socket](const QByteArray& frame) { return sendFrame(socket, frame); },
            [socket] { socket->abort(); });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] {
            const auto current = m_clients.find(socket);
            if (current != m_clients.end()) {
                current->second->handshakeTimer.stop();
                current->second->input->finish();
            }
            // abort() may emit disconnected synchronously from send(); defer
            // destruction past any callback still on the stack.
            QTimer::singleShot(0, this, [this, socket] { dropClient(socket); });
        });
        client->handshakeTimer.start();
    }
}

void RemoteControlServer::handshakeExpired(QWebSocket* socket)
{
    const auto current = m_clients.find(socket);
    if (current == m_clients.end()) { return; }
    Client& client = *current->second;
    if (client.input->awaitingReply() && !client.handshakeExtended) {
        // pairing.complete was accepted and its credential is being written.
        client.handshakeExtended = true;
        client.handshakeTimer.start(kPairingWriteTimeoutMs);
        return;
    }
    client.input->finish();
    refuse(socket, QStringLiteral("engine.timeout"), QStringLiteral("hello handshake timed out"));
}

void RemoteControlServer::receiveText(QWebSocket* socket, const QString& message)
{
    if (!m_serving) { return; }
    const auto current = m_clients.find(socket);
    if (current == m_clients.end()) { return; }
    Client& client = *current->second;
    const QByteArray bytes = message.toUtf8();
    // One request per frame: a line break would smuggle a second one in.
    // The pending bound keeps a client that outruns the pump from queueing
    // without limit (the per-session rate limit applies after hello).
    if (bytes.contains('\n') || bytes.contains('\r')
        || bytes.size() > ProtocolLimits::kMaxMessageBytes
        || client.pending.size() + bytes.size() + 1 > 2 * ProtocolLimits::kMaxMessageBytes) {
        client.input->finish();
        refuse(socket, QStringLiteral("transport.limit_exceeded"),
               QStringLiteral("one compact JSON request per text frame"));
        return;
    }
    client.pending.append(bytes);
    client.pending.append('\n');
    client.input->readAvailable();
}

void RemoteControlServer::dropClient(QWebSocket* socket)
{
    const auto current = m_clients.find(socket);
    if (current == m_clients.end()) { return; }
    const std::unique_ptr<Client> retired = std::move(current->second);
    m_clients.erase(current);
}

bool RemoteControlServer::send(QWebSocket* socket, const QJsonObject& message)
{
    return sendFrame(socket, QJsonDocument(message).toJson(QJsonDocument::Compact));
}

bool RemoteControlServer::sendFrame(QWebSocket* socket, QByteArray frame)
{
    const auto current = m_clients.find(socket);
    const auto fail = [&] {
        if (current != m_clients.end()) { current->second->input->finish(); }
        return false;
    };
    if (!socket || !socket->isValid()) { return fail(); }
    if (frame.endsWith('\n')) { frame.chop(1); }   // session frames are newline-delimited
    if (socket->bytesToWrite() + frame.size() > m_limits.maxQueuedOutputBytes) {
        fail();
        socket->abort();
        return false;
    }
    if (socket->sendTextMessage(QString::fromUtf8(frame)) != frame.size()) {
        fail();
        socket->abort();
        return false;
    }
    return true;
}

} // namespace AetherSDR::control
