#pragma once
#include "core/AetherPDInstallationCoordinator.h"
#include "core/AetherPDPackageDownload.h"
#include "core/AetherPDInstallationJournal.h"
#include <QFutureWatcher>
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
    // Nonempty attempt digest stores a new credential in a separate vault slot;
    // empty digest promotes it to the radio's active credential after commit.
    using TokenWriter = std::function<void(const QString&, const QString&, const QString&, QObject*, std::function<void(bool)>)>;
    using Factory = std::function<std::unique_ptr<Coordinator>(RadioModel&)>;
    using DownloadFactory = std::function<std::unique_ptr<AetherPDPackageDownload>()>;
    struct RecoveryServices {
        std::function<bool(const QByteArray&, const QByteArray&, bool)> journal;
        std::function<AetherPDInstallationJournal::Read(const QByteArray&)> read;
        std::function<void(const QString&, const QString&, QObject*, std::function<void(const QString&)>)> load;
        std::function<std::unique_ptr<AetherPDInstallationMonitor>()> monitor;
    };
    enum class State { Idle, Downloading, Preparing, AwaitingApproval, LoadingCredential, Running, Canceling, Finished, Unavailable, SavingCredential, PromotingCredential, LoadingRecoveryCredential, CheckingInstallation };
    Q_ENUM(State)
    enum class Failure { None, SessionChanged, NoCredential, CredentialTimeout, StartRefused, DownloadFailed, PackageRejected, CredentialSaveFailed, JournalFailed };
    Q_ENUM(Failure)
    explicit AetherPDInstallationModel(RadioModel& radio, QObject* parent = nullptr);
    AetherPDInstallationModel(RadioModel& radio, TokenLoader loader, Factory factory, QObject* parent = nullptr, DownloadFactory downloader = {}, TokenWriter writer = {}, RecoveryServices recovery = {});
    bool prepareRelease(const AetherPDRelease& release, const RadioControlSession& session);
    bool prepareFirstPackage(const AetherPDFirstPackage& package, const QString& hostname, const RadioControlSession& session);
    bool recoverFirstInstallation(const QByteArray& publisherKey, const RadioControlSession& session);
    bool recovering() const { return m_recovering; }
    QString selectedDevelopmentVersion() const;
    bool prepareSelectedDevelopment(const RadioControlSession& session);
    ~AetherPDInstallationModel() override;
    bool offer(const Coordinator::Prepared& package, const RadioControlSession& session);
    struct Preview {
        QByteArray plan;
        QString serial, firmware, previousDigest, candidateDigest, candidateKey, planDigest;
        QString carrierName, archiveSha256;
        bool firstInstallation{false};
        QString nativeName;
    };
    std::optional<Preview> preview() const;
    bool approve(const QByteArray& displayedPlan);
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
    qint64 downloadReceived() const { return m_received; }
    qint64 downloadTotal() const { return m_total; }
    Coordinator::State phase() const { return m_job ? m_job->state() : Coordinator::State::Idle; }
signals:
    void changed();
private:
    bool current() const;
    void unavailable(Failure reason);
    void retirePreparation();
    void downloaded(quint64 generation);
    void preparePackage(std::function<std::optional<AetherPDPackage>()> load, quint64 generation);
    void prepareSnapshot(std::function<std::optional<Coordinator::Prepared>()> prepare, quint64 generation);
    void credential(const QString& token, quint64 generation);
    void completed(Coordinator::Outcome result, bool cleanup);
    void promoted(bool persisted, quint64 generation);
    void recoveryCredential(const QString& token, quint64 generation);
    QPointer<RadioModel> m_radio;
    RadioControlSession m_session{};
    QHostAddress m_address;
    QString m_serial, m_firmware, m_carrier, m_archiveSha256;
    QByteArray m_plan, m_publisherKey;
    QString m_nativeName;
    TokenLoader m_load;
    TokenWriter m_write;
    RecoveryServices m_recovery;
    std::unique_ptr<AetherPDInstallationMonitor> m_recoveryMonitor;
    bool m_recovering{false};
    QString m_firstToken;
    Factory m_factory;
    DownloadFactory m_downloadFactory;
    std::unique_ptr<AetherPDPackageDownload> m_download;
    QPointer<QFutureWatcher<std::optional<Coordinator::Prepared>>> m_preparation;
    qint64 m_received{0}, m_total{0};
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
