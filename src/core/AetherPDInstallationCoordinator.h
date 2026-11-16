#pragma once

#include "AetherPDPackage.h"
#include "AetherPDInstallUpload.h"
#include "core/backends/FileTransferRequest.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <atomic>
#include <memory>
#include <optional>

class QNetworkAccessManager;
namespace AetherSDR {
class RadioModel;
class AetherPDCarrierUpload;
class AetherPDInstallRequestClient;
class AetherPDInstallationMonitor;

// One already-authenticated package transition and explicit operator approval.
// No package trust establishment, keychain lookup or RF. First installation
// requires the caller to persist the new credential before approved begin().
class AetherPDInstallationCoordinator final : public QObject {
    Q_OBJECT
public:
    class Prepared {
    public:
        QString carrierName() const { return m_image.containerName; }
        QString nativeName() const { return m_nativeName; }
        QByteArray publisherKey() const { return m_publisherKey; }
        QString carrierSha256() const { return m_image.archiveSha256; }
        QByteArray approvedPlan() const { return m_inputs[1]; }
        bool used() const { return m_started && m_started->load(); }
    private:
        friend class AetherPDInstallationCoordinator;
        AetherPDInstallUpload::Inputs m_inputs;
        aetherpd::CarrierImage m_image;
        QByteArray m_privateConfig;
        QByteArray m_nativeArchive, m_publisherKey;
        QString m_nativeName;
        std::shared_ptr<std::atomic_bool> m_started;
    };
    enum class State { Idle, Requesting, UploadingCarrier, WaitingForCarrier, UploadingInputs, WaitingForCommit, Cleaning, Finished, UploadingNative, WaitingForNative };
    Q_ENUM(State)
    enum class Outcome { Committed, RolledBack, Unconfirmed, Canceled };
    Q_ENUM(Outcome)
    using Clock = std::function<qint64()>;
    explicit AetherPDInstallationCoordinator(RadioModel& model, QObject* parent = nullptr);
    AetherPDInstallationCoordinator(RadioModel& model, QNetworkAccessManager& network,
                                   AetherPDInstallChannel& channel, AetherPDCarrierUpload& carrier, Clock clock, QObject* parent = nullptr);
    ~AetherPDInstallationCoordinator() override;
    // Pure local packaging, potentially expensive: call off the UI thread.
    // Accepts only verified package snapshots. Generates fresh RAM-only
    // key and attempt nonce; preview/approve these exact bytes before begin().
    static std::optional<Prepared> prepare(const AetherPDPackage& package, const QString& serial);
    static std::optional<Prepared> prepareFirst(const AetherPDFirstPackage& package, const QString& serial,
                                                const QByteArray& newTokenHash, const QString& hostname);
    static std::optional<Prepared> prepareSavedFirst(const AetherPDFirstPackage& package,
                                                    const QByteArray& savedPlan, const QByteArray& publisherKey);
    bool begin(const Prepared& approved, const RadioControlSession& expectedSession, const QString& token);
    // Reauthenticate the exact committed first plan before public removal.
    // No archive, installation request or upload can be issued by this mode.
    bool beginFirstCleanup(const QByteArray& plan, const QByteArray& publisherKey,
                           const RadioControlSession& expectedSession, const QString& token);
    void cancel();
    void poll();
    State state() const { return m_state; }
    QString carrierName() const { return m_prepared.m_image.containerName; }
    QString requestId() const;
    // Cleanup confirmation covers public removal/retirement observations, not
    // an independent host-process or cached-file audit.
    // finished(cleanupConfirmed=false) is never a complete installation claim.
    // Keep the coordinator alive through finished(); destruction abandons local
    // observation and cannot guarantee asynchronous radio cleanup.
signals:
    void stateChanged(AetherSDR::AetherPDInstallationCoordinator::State state);
    void finished(AetherSDR::AetherPDInstallationCoordinator::Outcome outcome, bool cleanupConfirmed);
private:
    void initialize();
    bool current() const;
    bool carrierPresent(bool containersOnly = false) const;
    void setState(State state);
    void startCarrier();
    void carrierFinished(bool success);
    void startInputs();
    void clean(Outcome outcome);
    void end(bool cleaned);
    void stopLocal();
    QPointer<RadioModel> m_model;
    RadioControlSession m_session{};
    FileTransferHandle m_reservation;
    Prepared m_prepared;
    QString m_serial, m_firmware;
    QString m_token;
    std::unique_ptr<AetherPDCarrierUpload> m_ownedCarrier;
    QPointer<AetherPDCarrierUpload> m_carrier;
    std::unique_ptr<AetherPDInstallRequestClient> m_request;
    std::unique_ptr<AetherPDInstallUpload> m_upload;
    std::unique_ptr<AetherPDInstallationMonitor> m_monitor;
    QTemporaryFile m_file;
    QTemporaryDir m_nativeDirectory;
    QString m_nativePath, m_carrierPath;
    QTimer m_timer;
    QElapsedTimer m_elapsed;
    Clock m_now;
    State m_state{State::Idle};
    Outcome m_outcome{Outcome::Unconfirmed};
    qint64 m_lastNow{0}, m_deadline{0}, m_nextInspect{0};
    bool m_requestStarted{false}, m_receiverRetired{false}, m_stopSent{false};
    qint64 m_nativeReadyAt{0};
    bool m_nativeStarted{false}, m_nativeRemoveSent{false}, m_nativeRemoved{false};
    bool m_carrierStarted{false}, m_carrierSeen{false}, m_removeSent{false};
    bool m_cleanupOnly{false}, m_cleanupRemoveAccepted{false};
};
} // namespace AetherSDR
