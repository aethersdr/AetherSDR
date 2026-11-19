#pragma once

#include "TailnetShimClient.h"
#include "TailnetShimTokenStore.h"

#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

namespace AetherSDR {

// One explicit operator action. Keep application-owned through finished(); a
// closed observer must not discard a replacement already selected by the radio.
class TailnetShimOwnerKeyTransaction final : public QObject {
    Q_OBJECT
public:
    using Entry = TailnetShimTokenStore::Entry;
    using Done = std::function<void(bool, const QString&)>;
    using Lease = std::shared_ptr<void>;
    using Acquire = std::function<Lease(const QString&)>;
    using Generate = std::function<QString()>;
    struct Credentials {
        std::function<bool()> available;
        std::function<void(const QString&, QObject*, std::function<void(const QString&)>, Entry)> load;
        std::function<quint64(const QString&, Entry)> revision;
        std::function<bool(const QString&, const QString&, quint64, Entry)> durable;
        std::function<bool(const QString&, quint64, const QString&, QObject*, Done, Entry)> create;
        std::function<bool(const QString&, const QString&, quint64, const QString&, QObject*, Done, Entry)> replace;
        std::function<bool(const QString&, const QString&, quint64, Entry, QObject*, std::function<void(bool)>)> retire;
    };
    struct Target {
        QString serial;
        QHostAddress address;
        TailnetShimStatus status;
        std::function<bool()> sessionCurrent;
    };
    enum class Action { Replace, Resume };
    enum class State { Idle, Loading, SavingCandidate, Confirming, Replacing, Promoting, Cleaning, Finished };
    Q_ENUM(State)
    enum class Outcome { Completed, CleanupUnconfirmed, Unconfirmed, Refused, SessionRetired };
    Q_ENUM(Outcome)
    enum class Reason { None, Unsupported, Busy, Storage, NoOwner, MissingCandidate, PendingCandidate,
                        ChangedCredential, Session, Remote, Deadline, Canceled };
    Q_ENUM(Reason)
    explicit TailnetShimOwnerKeyTransaction(QObject* parent = nullptr);
    TailnetShimOwnerKeyTransaction(QNetworkAccessManager& network, Credentials credentials, Acquire acquire,
                                   Generate generate, QObject* parent = nullptr, int deadlineMs = 30000);
    bool begin(const Target& target, Action action);
    void cancel();
    State state() const { return m_state; }
    Reason reason() const { return m_reason; }
    bool remoteRequestSent() const { return m_remoteRequestSent; }
    static Credentials systemCredentials();
signals:
    void stateChanged(AetherSDR::TailnetShimOwnerKeyTransaction::State state);
    void finished(AetherSDR::TailnetShimOwnerKeyTransaction::Outcome outcome,
                  AetherSDR::TailnetShimOwnerKeyTransaction::Reason reason);
private:
    void initialize(int deadlineMs);
    bool transition(State state);
    bool current();
    bool sameCredentials() const;
    void loadActive();
    void loadCandidate();
    void prepareCandidate(bool creating);
    void verifyCandidate();
    void submit(bool confirming);
    void promote();
    void verifyActive();
    void clean();
    void end(Outcome outcome, Reason reason);
    TailnetShimClient m_client;
    Credentials m_credentials;
    Acquire m_acquire;
    Generate m_generate;
    QTimer m_deadline;
    Lease m_lease;
    Target m_target;
    QString m_active, m_candidate, m_candidateId;
    quint64 m_activeRevision{0}, m_candidateRevision{0};
    Action m_action{Action::Replace};
    State m_state{State::Idle};
    Reason m_reason{Reason::None};
    bool m_finished{false}, m_confirming{false}, m_remoteRequestSent{false};
};
} // namespace AetherSDR
