#pragma once

#include "AetherPDProtocol.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

namespace AetherSDR {

// LAN transport for the shim's fixed PD operations. It neither
// owns RF controls nor retries requests whose effect may already have occurred.
class AetherPDClient : public QObject {
    Q_OBJECT
public:
    enum class Failure { InvalidRequest, Busy, Timeout, OversizedReply, Transport,
                         Unauthorized, Refused, InvalidReply };
    Q_ENUM(Failure)

    explicit AetherPDClient(QObject* parent = nullptr);
    // Inject our own HTTP transport in socket-free tests.
    explicit AetherPDClient(QNetworkAccessManager& network, QObject* parent = nullptr);
    ~AetherPDClient() override;

    void setRadioAddress(const QHostAddress& address);
    bool submit(aetherpd::Operation operation, const QString& adminToken,
                std::optional<aetherpd::StartIdentity> identity = std::nullopt);
    void cancelPending();
    bool busy() const { return !m_reply.isNull(); }

signals:
    void statusReceived(const AetherSDR::aetherpd::Status& status);
    void acknowledged(AetherSDR::aetherpd::Operation operation,
                      AetherSDR::aetherpd::Acknowledgment acknowledgment);
    void failed(AetherSDR::aetherpd::Operation operation, AetherSDR::AetherPDClient::Failure failure);
    // Local request abandonment; remote cancellation still needs an ACK or
    // observed state. A lost arm reply must not be treated as a canceled arm.
    void requestCanceled(AetherSDR::aetherpd::Operation operation);

private:
    void initialize();
    void finish(QNetworkReply* reply, aetherpd::Operation operation);

    QNetworkAccessManager m_ownedNetwork;
    QPointer<QNetworkAccessManager> m_network;
    QHostAddress m_address;
    QPointer<QNetworkReply> m_reply;
    aetherpd::Operation m_operation{aetherpd::Operation::Status};
    QTimer m_deadline;
    bool m_timedOut{false};
    bool m_oversized{false};
};

} // namespace AetherSDR
