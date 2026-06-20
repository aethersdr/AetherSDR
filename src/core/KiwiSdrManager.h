#pragma once

#include "KiwiSdrClient.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

namespace AetherSDR {

struct KiwiSdrAntennaProfile {
    QString id;
    QString name;
    QString endpoint;
    bool autoConnect{false};
    int waterfallCellDb{0};
    int waterfallFloorDb{0};
    int waterfallRate{0};
};

class KiwiSdrManager : public QObject {
    Q_OBJECT

public:
    explicit KiwiSdrManager(QObject* parent = nullptr);
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
    bool waterfallAvailable(const QString& id) const;
    QString waterfallDetail(const QString& id) const;
    bool isConnected(const QString& id) const;

    QString assignedProfileForSlice(int sliceId) const;
    int assignedSliceForProfile(const QString& id) const;

public slots:
    QString addProfile(const QString& name, const QString& endpoint);
    void updateProfile(const KiwiSdrAntennaProfile& profile);
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
                              double bandwidthMhz, int lineDurationMs);
    void assignSliceToProfile(int sliceId, const QString& profileId,
                              double frequencyMhz, const QString& mode,
                              int filterLowHz, int filterHighHz,
                              const QString& panId);
    void clearSliceAssignment(int sliceId);
    void updateSliceTracking(int sliceId, double frequencyMhz,
                             const QString& mode, int filterLowHz,
                             int filterHighHz, const QString& panId);
    void updateWaterfallView(int sliceId, const QString& panId,
                             double centerMhz, double bandwidthMhz,
                             int lineDurationMs);
    void setProfileWaterfallSettings(const QString& id, int cellDb,
                                     int floorDb, int rate);

signals:
    void profilesChanged();
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
    bool shouldMaintainProfileConnection(const QString& id) const;
    void scheduleReconnect(const QString& id);
    void cancelReconnect(const QString& id);
    static QString sanitizedName(const QString& name, const QString& endpoint);

    QVector<KiwiSdrAntennaProfile> m_profiles;
    QHash<QString, KiwiSdrClient*> m_clients;
    QHash<QString, QTimer*> m_reconnectTimers;
    QHash<QString, QString> m_stateDetails;
    QHash<QString, KiwiSdrReceiverTelemetry> m_telemetry;
    QHash<QString, bool> m_waterfallAvailable;
    QHash<QString, QString> m_waterfallDetails;
    QHash<int, QString> m_sliceAssignments;
    QString m_operatorCallsign;
};

} // namespace AetherSDR
