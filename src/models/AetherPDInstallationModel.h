#pragma once
#include "core/AetherPDInstallationCoordinator.h"
#include "core/AetherPDPackageDownload.h"
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
    using Factory = std::function<std::unique_ptr<Coordinator>(RadioModel&)>;
    using DownloadFactory = std::function<std::unique_ptr<AetherPDPackageDownload>()>;
    enum class State { Idle, Downloading, Preparing, AwaitingApproval, LoadingCredential, Running, Canceling, Finished, Unavailable };
    Q_ENUM(State)
    enum class Failure { None, SessionChanged, NoCredential, CredentialTimeout, StartRefused, DownloadFailed, PackageRejected };
    Q_ENUM(Failure)
    explicit AetherPDInstallationModel(RadioModel& radio, QObject* parent = nullptr);
    AetherPDInstallationModel(RadioModel& radio, TokenLoader loader, Factory factory, QObject* parent = nullptr, DownloadFactory downloader = {});
    bool prepareRelease(const AetherPDRelease& release, const RadioControlSession& session);
    QString selectedDevelopmentVersion() const;
    bool prepareSelectedDevelopment(const RadioControlSession& session);
    ~AetherPDInstallationModel() override;
    bool offer(const Coordinator::Prepared& package, const RadioControlSession& session);
    struct Preview {
        QByteArray plan;
        QString serial, firmware, previousDigest, candidateDigest, candidateKey, planDigest;
        QString carrierName, archiveSha256;
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
    void credential(const QString& token, quint64 generation);
    QPointer<RadioModel> m_radio;
    RadioControlSession m_session{};
    QHostAddress m_address;
    QString m_serial, m_firmware, m_carrier, m_archiveSha256;
    QByteArray m_plan;
    TokenLoader m_load;
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
