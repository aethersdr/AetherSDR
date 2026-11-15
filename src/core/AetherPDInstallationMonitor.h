#pragma once

#include "AetherPDProtocol.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <optional>

class QNetworkReply;

namespace AetherSDR {
// One approved transaction per instance, called only on its owning thread.
// Retries only the fixed status GET. Cancel/destruction abandon observation;
// neither cancels, retries, rolls back, nor starts an installation or RF session.
class AetherPDInstallationMonitor final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Waiting, Committed, RolledBack, Unconfirmed, Canceled };
    Q_ENUM(State)
    enum class Reason { None, Deadline, Unauthorized, InvalidReply, Mismatch, TransportDestroyed, InvalidClock };
    Q_ENUM(Reason)
    using Clock = std::function<qint64()>;
    explicit AetherPDInstallationMonitor(QObject* parent = nullptr);
    AetherPDInstallationMonitor(QNetworkAccessManager& network, Clock clock, QObject* parent = nullptr);
    ~AetherPDInstallationMonitor() override;
    bool begin(const QHostAddress& address, const QString& token, const QByteArray& approvedPlan,
               int timeoutMs = 180'000);
    void poll();
    void cancel();
    State state() const { return m_state; }
    Reason reason() const { return m_reason; }

signals:
    void finished(AetherSDR::AetherPDInstallationMonitor::State state,
                  AetherSDR::AetherPDInstallationMonitor::Reason reason);

private:
    void initialize();
    std::optional<qint64> now();
    void abandonReply();
    void end(State state, Reason reason);
    void send(qint64 stamp);
    void receive(QNetworkReply* reply);

    QNetworkAccessManager m_ownedNetwork;
    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timer;
    QElapsedTimer m_clock;
    Clock m_now;
    QHostAddress m_address;
    QString m_token;
    std::optional<aetherpd::InstallationPlan> m_plan;
    State m_state{State::Idle};
    Reason m_reason{Reason::None};
    qint64 m_lastNow{0};
    qint64 m_deadline{0};
    qint64 m_requestDeadline{0};
    qint64 m_nextPoll{0};
};
} // namespace AetherSDR
