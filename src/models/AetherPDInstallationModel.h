#pragma once
#include "core/AetherPDInstallationCoordinator.h"
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace AetherSDR {
class RadioModel;
// RadioModel owns this job, independently of any presenting dialog. Package
// authentication/preparation precedes offer(); approve() is explicit intent.
class AetherPDInstallationModel final : public QObject {
    Q_OBJECT
public:
    using Coordinator = AetherPDInstallationCoordinator;
    using TokenLoader = std::function<void(const QString&, QObject*, std::function<void(const QString&)>)>;
    using Factory = std::function<std::unique_ptr<Coordinator>(RadioModel&)>;
    enum class State { Idle, AwaitingApproval, LoadingCredential, Running, Canceling, Finished, Unavailable };
    Q_ENUM(State)
    enum class Failure { None, SessionChanged, NoCredential, CredentialTimeout, StartRefused };
    Q_ENUM(Failure)
    explicit AetherPDInstallationModel(RadioModel& radio, QObject* parent = nullptr);
    AetherPDInstallationModel(RadioModel& radio, TokenLoader loader, Factory factory, QObject* parent = nullptr);
    ~AetherPDInstallationModel() override;
    bool offer(const Coordinator::Prepared& package, const RadioControlSession& session);
    bool approve();
    void cancel();
    void reconcile();
    State state() const { return m_state; }
    Failure failure() const { return m_failure; }
    bool active() const;
    QString serial() const { return m_serial; }
    QString carrierName() const { return m_carrier; }
    QString archiveSha256() const { return m_archiveSha256; }
    QByteArray approvedPlan() const { return m_plan; }
    std::optional<Coordinator::Outcome> outcome() const { return m_outcome; }
    bool cleanupConfirmed() const { return m_cleanup; }
    Coordinator::State phase() const { return m_job ? m_job->state() : Coordinator::State::Idle; }
signals:
    void changed();
private:
    bool current() const;
    void unavailable(Failure reason);
    void credential(const QString& token, quint64 generation);
    QPointer<RadioModel> m_radio;
    RadioControlSession m_session{};
    QHostAddress m_address;
    QString m_serial, m_firmware, m_carrier, m_archiveSha256;
    QByteArray m_plan;
    TokenLoader m_load;
    Factory m_factory;
    QTimer m_timeout;
    quint64 m_generation{0};
    State m_state{State::Idle};
    Failure m_failure{Failure::None};
    std::optional<Coordinator::Prepared> m_package;
    std::unique_ptr<Coordinator> m_job;
    std::optional<Coordinator::Outcome> m_outcome;
    bool m_cleanup{false};
};
} // namespace AetherSDR
