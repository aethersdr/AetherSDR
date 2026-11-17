#pragma once
#include "AetherPDProtocol.h"
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
// One owner-authenticated LAN status GET. No retry, redirect, installation,
// package trust import, credential persistence or RF authority.
class AetherPDInstallationRead final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Reading, Ready, Failed, Canceled };
    Q_ENUM(State)
    enum class Failure { None, Network, Unauthorized, InvalidReply, Timeout, InvalidClock };
    Q_ENUM(Failure)
    using Clock = std::function<qint64()>;
    explicit AetherPDInstallationRead(QObject* parent = nullptr);
    AetherPDInstallationRead(QNetworkAccessManager& network, Clock clock = {}, QObject* parent = nullptr);
    ~AetherPDInstallationRead() override;
    bool begin(const QHostAddress& address, const QString& token);
    void cancel();
    State state() const { return m_state; }
    Failure failure() const { return m_failure; }
    std::optional<aetherpd::InstallationObservation> observation() const { return m_observation; }
signals:
    void finished();
private:
    void initialize();
    bool timely();
    bool consume();
    void receive();
    void retireReply();
    void end(State state, Failure failure = Failure::None);
    QNetworkAccessManager m_ownedNetwork;
    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    QElapsedTimer m_clock;
    Clock m_now;
    qint64 m_lastStamp{0}, m_expires{0};
    QByteArray m_body;
    std::optional<aetherpd::InstallationObservation> m_observation;
    State m_state{State::Idle};
    Failure m_failure{Failure::None};
};
} // namespace AetherSDR
