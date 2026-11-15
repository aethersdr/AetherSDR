#pragma once

#include "core/AetherPDProtocol.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QtGlobal>

#include <functional>
#include <optional>

class QNetworkReply;

namespace AetherSDR::txlin {

// One explicit test per instance. All calls/callbacks are owner-thread only.
// Availability is an input to the original TX gate, never a transmit grant.
// Ended means helper termination, not RF release. The owner requests Stop
// before destruction; abandoned transport still leaves the native fixed timer.
class AetherPDTestClient final : public QObject {
public:
    enum class State { Idle, Starting, Verifying, Observing, Retiring, Ended, Failed };
    using Clock = std::function<qint64()>;
    using Changed = std::function<void()>;
    explicit AetherPDTestClient(QObject* parent = nullptr);
    AetherPDTestClient(QNetworkAccessManager& network, Clock now, QObject* parent = nullptr);
    ~AetherPDTestClient() override;
    bool setSession(const QHostAddress& address, const QString& token, std::function<bool()> current);
    bool setChangedCallback(Changed changed);
    bool begin(int cleanupAfterMs);
    bool requestStop();
    void clearSession();
    void poll();
    // Rechecks session, observation age and fixed deadline. A false result is
    // permanent for this test. Do not gate TX using state() alone.
    qint64 remainingMs();
    // Latest cleanup-initiation estimate from reply receipt + reported time,
    // including one millisecond of clock quantization. Zero means unavailable.
    // This is NOT an RF-stop bound: the lease must budget unkey processing too.
    qint64 cleanupRemainingUpperMs();
    State state() const { return m_state; }
    const QString& error() const { return m_error; }
    const QString& testId() const { return m_testId; }

private:
    void initialize();
    bool current() const;
    std::optional<qint64> now();
    void abandon();
    void fail(const QString& reason);
    void notify();
    void send(aetherpd::TestOperation operation);
    void finish(QNetworkReply* reply, quint64 generation, aetherpd::TestOperation operation);

    QNetworkAccessManager m_ownedNetwork;
    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timer;
    QElapsedTimer m_clock;
    Clock m_now;
    Changed m_changed;
    QHostAddress m_address;
    QString m_token;
    std::function<bool()> m_current;
    State m_state{State::Idle};
    QString m_error;
    QString m_testId;
    quint64 m_generation{0};
    quint64 m_radioDeadline{0};
    qint64 m_lastNow{0};
    qint64 m_requestAt{0};
    qint64 m_deadline{0};
    qint64 m_cleanupUpper{0};
    qint64 m_observedAt{0};
    qint64 m_stopAt{0};
    int m_requestTimeout{0};
    int m_duration{0};
    int m_previousRemaining{0};
    bool m_used{false};
    bool m_stopSent{false};
};
} // namespace AetherSDR::txlin
