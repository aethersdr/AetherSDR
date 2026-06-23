#pragma once

#include "KiwiSdrProtocol.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <QtGlobal>

#include <memory>
#include <vector>

class QTimer;
#ifdef HAVE_WEBSOCKETS
class QNetworkAccessManager;
class QNetworkReply;
class QWebSocket;
#endif

namespace AetherSDR {

class Resampler;

struct KiwiSdrReceiverControls {
    bool agcEnabled{true};
    int agcGainDb{50};
    bool agcHang{false};
    int agcThresholdDb{-100};
    int agcDecayMs{1000};
    bool squelchEnabled{false};
    // Kiwi server squelch: 0 open/off; non-NBFM uses signed dB offset from
    // median RSSI floor, while NBFM uses the raw 1-99 noise squelch level.
    int squelchThresholdDb{20};
};

struct KiwiSdrReceiverTelemetry {
    int soundSequence{-1};
    float soundRssiDbm{0.0f};
    bool hasSoundRssi{false};
    quint64 soundSequenceGaps{0};
    int waterfallSequence{-1};
    quint64 waterfallSequenceGaps{0};
    int users{-1};
    double reportedFrequencyKhz{0.0};
    bool adcClipping{false};
    bool hasAdcClipping{false};
    bool gpsGood{false};
    bool hasGpsGood{false};
};

class KiwiSdrClient : public QObject {
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Error,
    };
    Q_ENUM(State)

    explicit KiwiSdrClient(QObject* parent = nullptr);
    ~KiwiSdrClient() override;

    State state() const { return m_state; }
    QString endpoint() const { return m_endpoint; }
    bool isConnected() const { return m_state == State::Connected; }
    bool audioActive() const { return m_audioActive; }
    bool hasTrackedSlice() const
    {
        return m_trackedSliceId >= 0 && m_trackedFrequencyMhz > 0.0;
    }
    KiwiSdrReceiverControls receiverControls() const { return m_receiverControls; }
    KiwiSdrReceiverTelemetry telemetry() const { return m_telemetry; }
    bool waterfallAvailable() const { return m_waterfallAvailable; }
    QString waterfallAvailabilityDetail() const
    {
        return m_waterfallAvailabilityDetail;
    }
    static QString normalizeEndpoint(const QString& endpoint);

public slots:
    void setOperatorCallsign(const QString& callsign);
    void setReceiverControls(const KiwiSdrReceiverControls& controls);
    void connectToEndpoint(const QString& endpoint);
    void disconnectFromEndpoint();
    void setTrackedSlice(int sliceId, double frequencyMhz,
                         const QString& mode, int filterLowHz,
                         int filterHighHz, const QString& panId);
    void setWaterfallView(const QString& panId, double centerMhz,
                          double bandwidthMhz);
    void setWaterfallLineDurationMs(int lineDurationMs);
    void setWaterfallDisplayAdjustments(int cellDb, int floorDb);
    void setWaterfallRateOverride(int rate);
    void setAudioActive(bool active);
    void setDecodeAudioWhenInactive(bool decode);

signals:
    void stateChanged(AetherSDR::KiwiSdrClient::State state,
                      const QString& detail);
    void audioActiveChanged(bool active);
    void trackedSliceChanged(int sliceId, double frequencyMhz,
                             const QString& mode, int filterLowHz,
                             int filterHighHz, const QString& panId);
    void decodedAudioReady(const QByteArray& pcm24kStereoFloat);
    void waterfallRowReady(const QString& panId, const QVector<float>& binsDbm,
                           double lowFreqMhz, double highFreqMhz,
                           quint32 timecode);
    void meterReadingReady(
        const AetherSDR::KiwiSdrProtocol::MeterReading& reading);
    void telemetryChanged();
    void waterfallAvailabilityChanged(bool available, const QString& detail);
    void recoverableDisconnect(const QString& detail);

private:
    enum class StreamKind {
        Sound,
        Waterfall,
    };

    struct StartupTraceState {
        bool authSent{false};
        bool badpNonzeroSeen{false};
        bool identUserSent{false};
        bool browserSent{false};
        bool compressionSent{false};
        bool arOkSent{false};
        bool arOkUsedActualAudioRate{false};
        bool serverDeClientSent{false};
        bool squelchSent{false};
        bool genattnSent{false};
        bool genSent{false};
        bool agcSent{false};
        bool modSent{false};
        bool keepaliveSentOnSound{false};
    };

    static bool parseEndpoint(const QString& endpoint, QString* host, quint16* port);
    static QString stateLabel(State state);
    static QString streamLabel(StreamKind stream);
    void setState(State state, const QString& detail);
    void cleanupSockets();
    void sendSoundSetupCommands();
    void sendSoundAudioRateAck();
    void sendSoundSampleRateCommands();
    void sendWaterfallSetupCommands();
    void sendTrackedSliceToServer();
    void sendReceiverControlsToServer();
    void sendWaterfallViewToServer();
    void sendWaterfallDisplayAdjustmentsToServer();
    QString kiwiIdentityCallsign() const;
    QString identityDiagnosticText() const;
    void sendSoundIdentityToServer();
    void sendWaterfallIdentityToServer();
    QString kiwiMode() const;
    int kiwiLowCutHz() const;
    int kiwiHighCutHz() const;
    bool isSupportedPcmSoundFrame(
        const QByteArray& frame,
        const KiwiSdrProtocol::SoundFrameHeader& header) const;
    bool parseWaterfallFrameHeader(const QByteArray& frame, quint32* start,
                                   int* zoom) const;
    QByteArray decodeSoundFrame(const QByteArray& frame);
    QVector<float> decodeWaterfallFrame(const QByteArray& frame) const;
    void handleBinaryMessage(StreamKind stream, const QByteArray& frame);
    void handleSoundFrame(const QByteArray& frame);
    void handleWaterfallFrame(const QByteArray& frame);
    void handleMessage(StreamKind stream, const QByteArray& frame);
    void handleTextMessage(StreamKind stream, const QString& text);
    bool updateWaterfallFftBins(int binCount);
    void updateWaterfallAvailability();
    void updateSoundTelemetry(const QByteArray& frame);
    void updateWaterfallTelemetry(const QByteArray& frame);
    void emitTelemetryChanged();
    void sendWaterfallRateToServer();
    void markSoundAudioReady();
    QString logEndpoint() const;
    QString setupTimeoutDetail() const;
    void sendKeepalive();
    void sendSoundCommand(const QString& command);
    void sendWaterfallCommand(const QString& command);
    void resetProtocolTrace();
    qint64 protocolTraceElapsedMs() const;
    void traceProtocolEvent(const QString& event);
    void traceConnectionInfo(const QString& scheme, quint16 socketPort,
                             const QString& sessionId,
                             const QString& origin,
                             const QString& soundUrl,
                             const QString& waterfallUrl);
    void traceOutboundCommand(StreamKind stream, const QString& command,
                              bool sent);
    void traceInboundText(StreamKind stream, const QString& text);
    void traceInboundBinary(StreamKind stream, const QByteArray& frame);
    void traceClose(StreamKind stream, int closeCode, const QString& reason);
    void updateStartupTraceForOutbound(StreamKind stream,
                                       const QString& command,
                                       bool sent);

#ifdef HAVE_WEBSOCKETS
    void handleSocketError(const QString& detail, bool transportEstablished);
    void startStatusPreflight();
    void handleStatusPreflightFinished(QNetworkReply* reply);
    void openWebSockets();
    bool retryWithSecureWebSocket(bool transportEstablished);
#endif

    State m_state{State::Disconnected};
    QString m_stateDetail;
    QString m_endpoint;
    QString m_host;
    QString m_operatorCallsign;
    QString m_lastSoundIdentityCallsign;
    QString m_lastWaterfallIdentityCallsign;
    quint16 m_port{0};
    bool m_audioActive{false};
    KiwiSdrReceiverControls m_receiverControls;
    KiwiSdrReceiverTelemetry m_telemetry;
    bool m_telemetryPending{false};
    int m_trackedSliceId{-1};
    double m_trackedFrequencyMhz{0.0};
    QString m_trackedMode;
    int m_trackedFilterLowHz{0};
    int m_trackedFilterHighHz{0};
    QString m_trackedPanId;
    bool m_userDisconnecting{false};
    bool m_secureWebSocket{false};
    bool m_secureWebSocketRetryAttempted{false};
    bool m_soundSocketConnected{false};
    bool m_waterfallSocketConnected{false};
    bool m_soundAudioReady{false};
    bool m_soundAudioRateAcked{false};
    bool m_soundSampleRateCommandsSent{false};
    bool m_soundSampleRatePending{false};
    bool m_soundFrameSeen{false};
    bool m_loggedSoundFrameShape{false};
    bool m_loggedWaterfallFrameShape{false};
    bool m_decodeAudioWhenInactive{true};
    double m_soundSampleRateHz{12000.0};
    QString m_soundAudioRateText;
    double m_soundResamplerRateHz{0.0};
    bool m_haveSoundAudioRate{false};
    bool m_haveSoundSampleRate{false};
    qint64 m_soundDiagWindowStartUtcMs{0};
    qint64 m_lastSoundFrameUtcMs{0};
    qint64 m_lastSoundKeepaliveSentUtcMs{0};
    quint64 m_soundDiagFrames{0};
    quint64 m_soundDiagBytes{0};
    quint64 m_soundDiagDecodedSamples{0};
    qint64 m_waterfallDiagWindowStartUtcMs{0};
    qint64 m_lastWaterfallFrameUtcMs{0};
    quint64 m_waterfallDiagFrames{0};
    quint64 m_waterfallDiagBytes{0};
    qint64 m_protocolTraceStartUtcMs{0};
    QStringList m_protocolTraceTail;
    QString m_lastOutboundCommand;
    QString m_lastInboundMsg;
    QString m_lastInboundFrameType;
    qint64 m_lastOutboundCommandUtcMs{0};
    qint64 m_lastInboundMsgUtcMs{0};
    qint64 m_lastInboundFrameUtcMs{0};
    StartupTraceState m_startupTrace;
    bool m_protocolSendFailed{false};
    double m_waterfallServerCenterMhz{15.0};
    double m_waterfallServerBandwidthMhz{30.0};
    QString m_waterfallViewPanId;
    double m_waterfallViewCenterMhz{0.0};
    double m_waterfallViewBandwidthMhz{0.0};
    bool m_waterfallRequestValid{false};
    QString m_waterfallRequestPanId;
    quint32 m_waterfallRequestStart{0};
    int m_waterfallRequestZoom{0};
    double m_waterfallRequestLowMhz{0.0};
    double m_waterfallRequestHighMhz{0.0};
    int m_waterfallZoomCap{14};
    int m_waterfallFftBins{1024};
    float m_waterfallMinDbm{-130.0f};
    float m_waterfallMaxDbm{-50.0f};
    int m_waterfallCellDb{0};
    int m_waterfallFloorDb{0};
    int m_waterfallRateOverride{0};
    int m_waterfallLineDurationMs{100};
    bool m_waterfallAvailable{true};
    QString m_waterfallAvailabilityDetail;
    int m_waterfallRxChannel{-1};
    int m_waterfallChannelCount{-1};
    std::unique_ptr<Resampler> m_soundResampler;
    QByteArray m_lastDecodedSoundPcm;
    QVector<float> m_lastWaterfallBins;
    QString m_lastWaterfallPanId;
    double m_lastWaterfallLowMhz{0.0};
    double m_lastWaterfallHighMhz{0.0};
    bool m_lastWaterfallRowValid{false};
    QTimer* m_keepaliveTimer{nullptr};
    QTimer* m_audioReadyTimer{nullptr};

#ifdef HAVE_WEBSOCKETS
    QNetworkAccessManager* m_statusNetworkAccessManager{nullptr};
    QNetworkReply* m_statusReply{nullptr};
    // Status preflight tries http first, then https (proxied/TLS-only Kiwis)
    // before giving up.  ext_api can't be confirmed unless one succeeds.
    bool m_statusPreflightSecure{false};
    int m_statusPreflightFirstHttpStatus{0};
    QString m_statusPreflightFirstError;
    QWebSocket* m_soundSocket{nullptr};
    QWebSocket* m_waterfallSocket{nullptr};
#endif
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::KiwiSdrReceiverTelemetry)
