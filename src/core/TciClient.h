#pragma once
#ifdef HAVE_WEBSOCKETS

#include "TxCoordinator.h"
#include <QAbstractSocket>
#include <QHostAddress>
#include <QObject>
#include <QWebSocketProtocol>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace AetherSDR {

// Shared revocation only, never shared QObject state. The I/O owner retires
// this before notifying the controller of a disconnect. Client ids are never
// reused during a server's lifetime, including stop/start.
struct TciClientLifetime {
    inline static std::atomic<quint64> nextId{1};
    const quint64 id{nextId.fetch_add(1, std::memory_order_relaxed)};
    std::atomic<bool> live{true};
};

// Model-thread client identity and endpoint snapshot. This is NOT a socket;
// the transport exchanges ids and owning values with it, never QObject pointers.
class TciClient final : public QObject {
    Q_OBJECT
public:
    explicit TciClient(QObject* parent = nullptr) : QObject(parent) {}
    ~TciClient() override { lifetime->live.store(false, std::memory_order_release); }
    std::shared_ptr<TciClientLifetime> lifetime = std::make_shared<TciClientLifetime>();
    std::optional<TxCoordinator::Request> ingressRequest;
    QHostAddress address;
    quint16 endpointPort{0};
    QString lastError;
    QWebSocketProtocol::CloseCode lastClose{QWebSocketProtocol::CloseCodeNormal};
    std::function<qint64(const QString&)> textSink;
    std::function<qint64(const QByteArray&)> binarySink;
    std::function<void(QWebSocketProtocol::CloseCode, const QString&)> closeSink;

    quint64 id() const { return lifetime->id; }
    bool live() const { return lifetime->live.load(std::memory_order_acquire); }
    QHostAddress peerAddress() const { return address; }
    quint16 peerPort() const { return endpointPort; }
    QString errorString() const { return lastError; }
    QWebSocketProtocol::CloseCode closeCode() const { return lastClose; }
    QAbstractSocket::SocketState state() const
    { return live() ? QAbstractSocket::ConnectedState : QAbstractSocket::UnconnectedState; }
    qint64 sendTextMessage(const QString& message)
    { return live() && textSink ? textSink(message) : -1; }
    qint64 sendBinaryMessage(const QByteArray& message)
    { return live() && binarySink ? binarySink(message) : -1; }
    void close(QWebSocketProtocol::CloseCode code = QWebSocketProtocol::CloseCodeNormal,
               const QString& reason = {})
    {
        lifetime->live.store(false, std::memory_order_release);
        if (closeSink) { closeSink(code, reason); }
    }

signals:
    void textMessageReceived(const QString& message);
    void binaryMessageReceived(const QByteArray& message);
    void disconnected();
    void errorOccurred(QAbstractSocket::SocketError error);
};
} // namespace AetherSDR
#endif
