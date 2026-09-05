#pragma once

#include "ControlService.h"

#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QString>

#include <memory>
#include <unordered_map>

class QLockFile;
class QLocalSocket;

namespace AetherSDR::control {

class LocalControlServer final : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxClients = 8;
    static constexpr int kHandshakeTimeoutMs = 5000;
    static constexpr qint64 kMaxQueuedOutputBytes = 4 * 1024 * 1024;

    struct Limits {
        int maxClients{kMaxClients};
        int handshakeTimeoutMs{kHandshakeTimeoutMs};
        qint64 maxQueuedOutputBytes{kMaxQueuedOutputBytes};
    };

    explicit LocalControlServer(QObject* parent = nullptr);
    LocalControlServer(QObject* parent, Limits limits);
    ~LocalControlServer() override;

    [[nodiscard]] bool listen(const QString& name);
    void close();
    [[nodiscard]] bool isListening() const { return m_server.isListening(); }
    [[nodiscard]] QString fullServerName() const { return m_server.fullServerName(); }
    [[nodiscard]] ControlResourceStore& resourceStore() { return m_resources; }

private:
    struct Client;

    void acceptConnections();
    void readClient(QLocalSocket* socket);
    void drainSessionOutput(QLocalSocket* socket);
    void dropClient(QLocalSocket* socket);
    [[nodiscard]] bool send(QLocalSocket* socket, const QJsonObject& message);
    // Writes a frame the session already encoded, so an event is serialized
    // exactly once between ControlSession and the socket.
    [[nodiscard]] bool sendFrame(QLocalSocket* socket, const QByteArray& frame);
    [[nodiscard]] static bool resolveEndpoint(
        const QString& logicalName, QString* endpointName, QString* lockPath);

    QLocalServer m_server;
    ControlResourceStore m_resources;
    ControlService m_service;
    Limits m_limits;
    std::unordered_map<QLocalSocket*, std::unique_ptr<Client>> m_clients;
    std::unique_ptr<QLockFile> m_lock;
};

} // namespace AetherSDR::control
