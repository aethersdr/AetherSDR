#pragma once

#include "KiwiSdrClient.h"
#include "KiwiSdrCredentialStore.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

class QTimer;
class QThread;

namespace AetherSDR {

struct KiwiSdrAntennaProfile {
    QString id;
    QString name;
    QString endpoint;
    bool autoConnect{false};
    bool waterfallAutoScale{true};
    int waterfallMinDbm{-110};
    int waterfallMaxDbm{-10};
    int waterfallRate{0};
};

struct KiwiSdrWaterfallDisplayRange {
    float minDbm{0.0f};
    float maxDbm{0.0f};
    bool autoRange{false};
    bool valid{false};
};

enum class KiwiSdrPasswordPersistenceState {
    Loading,
    NoPassword,
    Saving,
    Stored,
    SessionOnly,
    Error,
};

class KiwiSdrManager : public QObject {
    Q_OBJECT

public:
    explicit KiwiSdrManager(
        QObject* parent = nullptr,
        std::shared_ptr<IKiwiSdrCredentialStore> credentialStore = {});
    ~KiwiSdrManager() override;

    QVector<KiwiSdrAntennaProfile> profiles() const { return m_profiles; }
    KiwiSdrAntennaProfile profile(const QString& id) const;
    bool hasProfile(const QString& id) const;
    QString displayName(const QString& id) const;
    QString virtualAntennaToken(const QString& id) const;
    QString profileIdForVirtualAntennaToken(const QString& token) const;
    QStringList virtualAntennaTokens() const;
    QStringList virtualAntennaLabels() const;

    KiwiSdrClient::State state(const QString& id) const;
    QString stateDetail(const QString& id) const;
    KiwiSdrReceiverTelemetry telemetry(const QString& id) const;
    KiwiSdrProtocol::ReceiverMetadata receiverMetadata(const QString& id) const;
    KiwiSdrProtocol::ProtocolState protocolState(const QString& id) const;
    bool waterfallAvailable(const QString& id) const;
    QString waterfallDetail(const QString& id) const;
    KiwiSdrWaterfallDisplayRange waterfallDisplayRange(
        const QString& id) const;
    bool isConnected(const QString& id) const;
    bool reconnectRecommended(const QString& id) const;

    QString assignedProfileForSlice(int sliceId) const;
    int assignedSliceForProfile(const QString& id) const;
    QString profilePassword(const QString& id) const;
    bool isProfilePasswordLoaded(const QString& id) const;
    KiwiSdrPasswordPersistenceState profilePasswordPersistenceState(
        const QString& id) const;
    QString profilePasswordPersistenceDetail(const QString& id) const;

public slots:
    QString addProfile(const QString& name, const QString& endpoint);
    void updateProfile(const KiwiSdrAntennaProfile& profile);
    void setProfilePassword(const QString& id, const QString& password);
    void removeProfile(const QString& id);
    void connectProfile(const QString& id);
    void disconnectProfile(const QString& id);
    void disconnectAll();
    void setOperatorCallsign(const QString& callsign);
    void startAutoConnect();
    void primeProfileTracking(const QString& id, int sliceId,
                              double frequencyMhz, const QString& mode,
                              int filterLowHz, int filterHighHz,
                              const QString& panId, double centerMhz,
                              double bandwidthMhz, int lineDurationMs,
                              const QString& bandName);
    void assignSliceToProfile(int sliceId, const QString& profileId,
                              double frequencyMhz, const QString& mode,
                              int filterLowHz, int filterHighHz,
                              const QString& panId,
                              const QString& bandName);
    void clearSliceAssignment(int sliceId);
    void updateSliceTracking(int sliceId, double frequencyMhz,
                             const QString& mode, int filterLowHz,
                             int filterHighHz, const QString& panId,
                             const QString& bandName);
    void updateWaterfallView(int sliceId, const QString& panId,
                             double centerMhz, double bandwidthMhz,
                             int lineDurationMs);
    void setReceiverControlsForSlice(
        int sliceId, const KiwiSdrReceiverControls& controls);
    void setProfileWaterfallDisplayRange(const QString& id, int minDbm,
                                         int maxDbm, bool autoScale,
                                         int rate);
    void requestProfileWaterfallAutoScale(const QString& id);

signals:
    void profilesChanged();
    void profilePasswordChanged(const QString& id);
    void profilePasswordPersistenceChanged(
        const QString& id,
        AetherSDR::KiwiSdrPasswordPersistenceState state,
        const QString& detail);
    void profileStateChanged(const QString& id, AetherSDR::KiwiSdrClient::State state,
                             const QString& detail);
    void profileTelemetryChanged(
        const QString& id,
        const AetherSDR::KiwiSdrReceiverTelemetry& telemetry);
    void profileWaterfallAvailabilityChanged(const QString& id,
                                             bool available,
                                             const QString& detail);
    void profileStreamReset(const QString& id);
    void sliceAssignmentChanged(int sliceId, const QString& profileId);
    void audioSourceEnabledChanged(const QString& id, bool enabled);
    // Emitted when a profile is removed entirely, so the audio engine can free
    // the per-source DSP state (disabling alone only quiesces it — #3668 review).
    void audioSourceRemoved(const QString& id);
    void decodedAudioReady(const QString& id, const QByteArray& pcm24kStereoFloat);
    void waterfallRowReady(const QString& id, const QString& panId,
                           const QVector<float>& binsDbm,
                           double lowFreqMhz, double highFreqMhz,
                           quint32 timecode);
    void waterfallDisplayRangeChanged(const QString& id, float minDbm,
                                      float maxDbm, bool autoRange);
    void meterReadingReady(
        const QString& id,
        const AetherSDR::KiwiSdrProtocol::MeterReading& reading);
    void profileNeedsInitialTracking(const QString& id);

private:
    KiwiSdrClient* ensureClient(const QString& id);
    KiwiSdrClient* client(const QString& id) const;
    int profileIndex(const QString& id) const;
    void loadSettings();
    void saveSettings() const;
    void loadProfilePassword(const QString& id);
    void deleteProfilePassword(const QString& id);
    void queueProfilePasswordStore(const QString& id, const QString& password,
                                   bool profileRemoval);
    void setProfilePasswordPersistence(
        const QString& id, KiwiSdrPasswordPersistenceState state,
        const QString& detail = {});
    static QString profilePasswordKey(const QString& id);
    bool shouldMaintainProfileConnection(const QString& id) const;
    void ensureClientThread();
    void invokeClient(const QString& id, std::function<void(KiwiSdrClient*)> fn);
    void destroyClient(const QString& id, bool blocking = false);
    void scheduleReconnect(const QString& id);
    void scheduleWaitingReconnectIfRecommended(const QString& id);
    void cancelReconnect(const QString& id);
    static QString sanitizedName(const QString& name, const QString& endpoint);

    QVector<KiwiSdrAntennaProfile> m_profiles;
    QHash<QString, KiwiSdrClient*> m_clients;
    QHash<QString, KiwiSdrClient::State> m_states;
    QHash<QString, bool> m_clientHasTrackedSlice;
    QHash<QString, QTimer*> m_reconnectTimers;
    QHash<QString, QString> m_stateDetails;
    QHash<QString, KiwiSdrReceiverTelemetry> m_telemetry;
    QHash<QString, bool> m_waterfallAvailable;
    QHash<QString, QString> m_waterfallDetails;
    QHash<QString, KiwiSdrWaterfallDisplayRange> m_waterfallDisplayRanges;
    QHash<int, QString> m_sliceAssignments;
    QHash<QString, QString> m_profilePasswords;
    QHash<QString, quint64> m_profilePasswordRevisions;
    struct PendingPasswordStore {
        quint64 revision{0};
        QString password;
        bool profileRemoval{false};
    };
    QHash<QString, KiwiSdrPasswordPersistenceState>
        m_profilePasswordPersistenceStates;
    QHash<QString, QString> m_profilePasswordPersistenceDetails;
    QSet<QString> m_loadedProfilePasswords;
    QSet<QString> m_loadingProfilePasswords;
    QSet<QString> m_pendingPasswordConnects;
    std::shared_ptr<IKiwiSdrCredentialStore> m_credentialStore;
    QString m_operatorCallsign;
    QThread* m_clientThread{nullptr};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::KiwiSdrPasswordPersistenceState)
