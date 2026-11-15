#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

class QNetworkReply;

namespace AetherSDR {
// One already-approved upgrade request per instance. No artifact verification,
// consent, upload, commit claim or RF. The coordinator supplies the approved
// plan and RAM-only private configuration, and separately observes commit.
class AetherPDInstallRequestClient final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Unknown, AwaitingInputs, Running, StopRequested, DeliveryFinished, Unavailable, Missing, Expired };
    Q_ENUM(State)
    enum class Reason { Deadline, Unauthorized, Busy, InvalidReply, Transport, TransportDestroyed, InvalidClock };
    Q_ENUM(Reason)
    using Clock = std::function<qint64()>;

    explicit AetherPDInstallRequestClient(QObject* parent = nullptr);
    AetherPDInstallRequestClient(QNetworkAccessManager& network, Clock clock, QObject* parent = nullptr);
    ~AetherPDInstallRequestClient() override;

    static std::optional<QByteArray> publicRecord(const QByteArray& privateConfig, const QByteArray& approvedPlan);
    bool begin(const QHostAddress& address, const QString& token,
               const QByteArray& privateConfig, const QByteArray& approvedPlan);
    bool inspect();
    // Requests only receiver retirement; it cannot undo a handed-off installer.
    bool requestStop();
    void poll();
    State state() const { return m_state; }
    bool busy() const { return !m_reply.isNull(); }
    QString requestId() const { return m_requestId; }

signals:
    void observed(AetherSDR::AetherPDInstallRequestClient::State state, int deliveryResult);
    // Unknown means no acknowledgement, not that the server did nothing.
    void failed(AetherSDR::AetherPDInstallRequestClient::Reason reason);

private:
    enum class Operation { Begin, Inspect, Stop };
    void initialize();
    std::optional<qint64> now();
    bool followup(Operation operation);
    void send(Operation operation, qint64 stamp, const QByteArray& body = {});
    void receive(QNetworkReply* reply);
    void abandonReply();
    void fail(Reason reason, bool expire = false);

    QNetworkAccessManager m_ownedNetwork;
    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QElapsedTimer m_clock;
    QTimer m_timer;
    Clock m_now;
    QHostAddress m_address;
    QString m_token;
    QString m_requestId;
    State m_state{State::Idle};
    Operation m_operation{Operation::Begin};
    qint64 m_lastNow{0};
    qint64 m_deadline{0};
    qint64 m_requestDeadline{0};
};
} // namespace AetherSDR
