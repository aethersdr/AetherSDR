#pragma once

#include "ControlEngine.h"

#include <QHostAddress>
#include <QObject>
#include <QSslConfiguration>
#include <QString>

#include <memory>
#include <optional>
#include <unordered_map>

class QWebSocket;
class QWebSocketServer;

namespace AetherSDR::control {

// The network listener (aetherd-remote-clients.md §4): WebSocket sessions of
// the same ControlEngine the local socket serves. One compact JSON object per
// text frame. Every session starts unauthenticated and is authorized only by a
// remote-scope credential at hello, or makes one pairing.complete attempt.
// Network sessions never reach transmit (ControlService enforces that).
// Built only with HAVE_WEBSOCKETS.
class RemoteControlServer final : public QObject {
    Q_OBJECT

public:
    enum class Mode {
        Secure,   // wss:// with a host-made certificate; any non-wildcard address
        Tailnet,  // plain ws:// on a WireGuard interface's address only
    };
    static constexpr int kMaxClients = 4;   // apart from the local socket's 8
    static constexpr int kHandshakeTimeoutMs = 5000;
    // A pairing whose credential is being written may run past the handshake
    // deadline once, up to this long, so a stored credential reaches its client.
    static constexpr int kPairingWriteTimeoutMs = 30000;
    static constexpr qint64 kMaxQueuedOutputBytes = 4 * 1024 * 1024;

    struct Limits {
        int maxClients{kMaxClients};
        int handshakeTimeoutMs{kHandshakeTimeoutMs};
        qint64 maxQueuedOutputBytes{kMaxQueuedOutputBytes};
    };
    struct Endpoint {
        QHostAddress address;
        quint16 port{0};
    };

    RemoteControlServer(ControlEngine& engine, Mode mode, QObject* parent = nullptr);
    RemoteControlServer(ControlEngine& engine, Mode mode, QObject* parent, Limits limits);
    ~RemoteControlServer() override;

    // "192.0.2.7:8443" or "[2001:db8::7]:8443". Nullopt for anything else,
    // including a host name, a missing port and port 0.
    [[nodiscard]] static std::optional<Endpoint> parseEndpoint(const QString& text);
    // The name of the WireGuard interface (tailscale* or wg*) that holds this
    // address and is up, or empty.
    [[nodiscard]] static QString wireGuardInterfaceFor(const QHostAddress& address);
    // Loads a PEM certificate and private key into a server TLS configuration
    // (TLS 1.2 or newer). Nullopt with *error set when either is unusable.
    [[nodiscard]] static std::optional<QSslConfiguration> loadTlsIdentity(
        const QString& certificatePath, const QString& keyPath, QString* error);
    // Lowercase hex SHA-256 of the certificate's DER form: what a client pins
    // and what the pairing code's check digits are derived from.
    [[nodiscard]] static QString certificateSha256(const QSslConfiguration& configuration);

    // Secure mode only, before listen().
    [[nodiscard]] bool setTlsConfiguration(const QSslConfiguration& configuration);
    // Refuses a wildcard address in either mode, and in Tailnet mode any
    // address not on an up WireGuard interface. Never falls back to another
    // address. Arrivals are closed until startServing().
    [[nodiscard]] bool listen(const Endpoint& endpoint, QString* error);
    [[nodiscard]] bool startServing();
    void close();
    [[nodiscard]] bool isListening() const;
    [[nodiscard]] quint16 serverPort() const;
    [[nodiscard]] std::size_t sessionCount() const { return m_clients.size(); }

private:
    struct Client;

    void acceptConnections();
    void receiveText(QWebSocket* socket, const QString& message);
    void dropClient(QWebSocket* socket);
    void handshakeExpired(QWebSocket* socket);
    [[nodiscard]] bool send(QWebSocket* socket, const QJsonObject& message);
    [[nodiscard]] bool sendFrame(QWebSocket* socket, QByteArray frame);
    void refuse(QWebSocket* socket, const QString& code, const QString& message);

    ControlEngine& m_engine;
    const Mode m_mode;
    Limits m_limits;
    std::unique_ptr<QWebSocketServer> m_server;
    bool m_tlsReady{false};
    std::unordered_map<QWebSocket*, std::unique_ptr<Client>> m_clients;
    bool m_serving{false};
    bool m_closing{false};
};

} // namespace AetherSDR::control
