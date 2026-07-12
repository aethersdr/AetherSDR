#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <QtGlobal>

namespace AetherSDR::KiwiSdrProtocol {

inline constexpr int kSquelchOffLevel = 0;
inline constexpr int kSquelchUiMinLevel = 0;
inline constexpr int kSquelchUiMaxLevel = 99;
inline constexpr int kSquelchServerMinMarginDb = -99;
inline constexpr int kSquelchServerMaxMarginDb = 99;
inline constexpr double kSquelchDefaultTailSeconds = 0.0;
inline constexpr int kAgcThresholdMinDb = -160;
inline constexpr int kAgcThresholdMaxDb = 0;
inline constexpr int kAgcManualGainMinDb = 0;
inline constexpr int kAgcManualGainMaxDb = 100;
inline constexpr int kAgcDecayMinMs = 20;
inline constexpr int kAgcDecayMaxMs = 5000;
inline constexpr int kAgcFastDecayMs = 300;
inline constexpr int kAgcMedDecayMs = 1000;
inline constexpr int kAgcSlowDecayMs = 3000;
inline constexpr int kAgcSlopeDb = 6;

struct SoundFrameHeader {
    int sequence{-1};
    quint8 flags{0};
    float rssiDbm{0.0f};
    bool hasRssi{false};
    bool squelched{false};
    bool valid{false};
};

struct WaterfallLineHeader {
    int sequence{-1};
    bool valid{false};
};

struct WaterfallAperture {
    float minDbm{0.0f};
    float maxDbm{0.0f};
    bool valid{false};
};

struct WaterfallDisplayRange {
    float minDbm{0.0f};
    float maxDbm{0.0f};
    bool valid{false};
};

struct IpLimitNotice {
    int minutes{0};
    QString address;
    bool valid{false};
};

struct MsgToken {
    QString key;
    QString value;
    bool hasValue{false};
};

enum class ApiPolicy {
    Unknown,
    Disabled,
    Limited,
    Open,
};

enum class CampStatus {
    Unknown,
    Offered,
    Queued,
    Accepted,
    Rejected,
    AudioStopped,
    Disconnected,
};

enum class AuthMode {
    Unknown,
    PublicPasswordless,
    PasswordRequired,
    Rejected,
};

enum class StreamMode {
    Unknown,
    Sound,
    Waterfall,
    Extension,
    Iq,
    Gnss,
};

enum class FrameLayout {
    Unknown,
    SndPcm16,
    SndObservedPcm16WithMeter,
    SndCompressed,
    WaterfallDirectBins,
    WaterfallCompactEncoded,
    Extension,
    UnknownBinary,
    Unsupported,
};

struct FrameObservation {
    StreamMode stream{StreamMode::Unknown};
    FrameLayout layout{FrameLayout::Unknown};
    int frameBytes{0};
    int payloadBytes{0};
    bool supported{false};
    QString unsupportedReason;
};

struct WaterfallFrameDecodeResult {
    FrameObservation observation;
    QVector<float> binsDbm;
};

struct SoundAdpcmState {
    int predictor{0};
    int index{0};
    bool valid{true};
};

struct SoundFrameDecodeResult {
    FrameObservation observation;
    QVector<float> monoSamples;
    bool decoderReset{false};
};

struct StreamCapability {
    StreamCapability() = default;
    explicit StreamCapability(StreamMode streamMode) : mode(streamMode) {}

    StreamMode mode{StreamMode::Unknown};
    bool requested{false};
    bool observed{false};
    bool uncompressedRequested{false};
    bool compressedRequested{false};
    bool uncompressedObserved{false};
    bool compressedObserved{false};
    QVector<FrameLayout> supportedLayouts;
    QVector<FrameLayout> observedLayouts;
    FrameLayout lastObservedLayout{FrameLayout::Unknown};
    QString unsupportedReason;
};

struct ProtocolState {
    QString serverVersion;
    QString serverBuild;
    AuthMode authMode{AuthMode::Unknown};
    ApiPolicy apiPolicy{ApiPolicy::Unknown};
    int extApi{-1};
    StreamCapability sound{StreamMode::Sound};
    StreamCapability waterfall{StreamMode::Waterfall};
    QStringList unsupportedFeatureReasons;
    QVector<FrameObservation> unsupportedFrames;
};

struct ReceiverMetadata {
    QString serverHeader;
    QString serverVersion;
    QString serverBuild;
    int users{-1};
    bool hasUsers{false};
    int usersMax{-1};
    bool hasUsersMax{false};
    int preempt{-1};
    bool hasPreempt{false};
    int extApi{-1};
    bool hasExtApi{false};
    ApiPolicy apiPolicy{ApiPolicy::Unknown};
    bool busy{false};
    bool hasBusy{false};
    CampStatus campStatus{CampStatus::Unknown};
    bool hasCampStatus{false};
    int campReceiverChannel{-1};
    bool hasCampReceiverChannel{false};
    int campQueuePosition{-1};
    bool hasCampQueuePosition{false};
    int campQueueWaiters{-1};
    bool hasCampQueueWaiters{false};
    bool campQueueReloadRecommended{false};
    bool hasCampQueueReloadRecommended{false};
    int maxCampers{-1};
    bool hasMaxCampers{false};
    bool gpsGood{false};
    bool hasGpsGood{false};
    QString gpsStatus;
    bool adcClipping{false};
    bool hasAdcClipping{false};
    double reportedFrequencyKhz{0.0};
    bool hasReportedFrequency{false};
    double coverageCenterMhz{0.0};
    bool hasCoverageCenter{false};
    double coverageBandwidthMhz{0.0};
    bool hasCoverageBandwidth{false};
    int receiverChannel{-1};
    bool hasReceiverChannel{false};
    int waterfallChannels{-1};
    bool hasWaterfallChannels{false};
    QStringList stableStatusFields;
};

enum class MeterSource {
    Unknown,
    SndMetadata,
    AudioSamples,
    WaterfallBins,
    MsgMetadata,
    ManualTest,
};

enum class MeterCapability {
    Unavailable,
    RelativeAudio,
    RelativeWaterfall,
    RawSndMeter,
    CalibratedSndMeter,
    Experimental,
};

enum class MeterConfidence {
    None,
    Low,
    Medium,
    High,
    Verified,
};

struct MeterContext {
    QString mode;
    double audioRateHz{0.0};
    double sampleRateHz{0.0};
    bool compressionRequested{false};
    bool compressionObserved{false};
    QString serverVersion;
    QString streamState;
};

struct MeterReading {
    qint64 timestampUtcMs{0};
    MeterSource source{MeterSource::Unknown};
    MeterCapability capability{MeterCapability::Unavailable};
    float rawValue{0.0f};
    bool hasRawValue{false};
    float dbm{0.0f};
    bool hasDbm{false};
    QString sUnits;
    float relativeLevel{0.0f};
    bool hasRelativeLevel{false};
    bool squelchStateKnown{false};
    bool squelched{false};
    bool valid{false};
    MeterConfidence confidence{MeterConfidence::None};
    QString label{QStringLiteral("Meter unavailable")};
    QString notes;
};

SoundFrameHeader parseSoundFrameHeader(const QByteArray& frame);
WaterfallLineHeader parseWaterfallLineHeader(const QByteArray& frame);
quint64 sequenceGapCount(int previousSequence, int currentSequence);
float waterfallByteToDbm(unsigned char value);
float waterfallByteToDisplayLevel(unsigned char value);
float calibratedWaterfallLevel(float dbm, int calibrationDb);
WaterfallAperture autoWaterfallAperture(const QVector<float>& binsDbm);
WaterfallDisplayRange autoWaterfallDisplayRange(const QVector<float>& binsDbm);
WaterfallDisplayRange autoWaterfallDisplayRangeFromRows(
    const QVector<QVector<float>>& rowsDbm);
float waterfallZoomCorrectionDb(int zoom);
WaterfallDisplayRange defaultWaterfallDisplayRange(int zoom,
                                                   int ceilingOffsetDb,
                                                   int floorOffsetDb);
WaterfallDisplayRange adjustedWaterfallDisplayRange(float minDbm,
                                                    float maxDbm,
                                                    int ceilingOffsetDb,
                                                    int floorOffsetDb);
WaterfallDisplayRange zoomAdjustedWaterfallDisplayRange(float minDbm,
                                                        float maxDbm,
                                                        int sourceZoom,
                                                        int currentZoom);
float waterfallColorIndex(float dbm, float minDbm, float maxDbm);
QVector<MsgToken> parseMsgTokens(const QString& message);
IpLimitNotice parseIpLimitNotice(const QString& valueText);
ApiPolicy apiPolicyFor(int extApi, int usersMax);
QString apiPolicyName(ApiPolicy policy);
QString campStatusName(CampStatus status);
QString authModeName(AuthMode mode);
QString streamModeName(StreamMode mode);
QString frameLayoutName(FrameLayout layout);
ProtocolState defaultProtocolState();
bool diagnosticCompressionFlagEnabled(const QByteArray& value);
bool diagnosticWaterfallCompressionFlagEnabled(const QByteArray& value);
QString formatSoundCompressionCommand(bool compressed);
QString formatWaterfallCompressionCommand(bool compressed);
constexpr int kAuthPasswordEncodedMaxLength = 256;
bool authPasswordFitsServerLimit(const QString& password);
QString formatAuthCommand(const QString& password);
FrameObservation classifySoundFrame(const QByteArray& frame);
FrameObservation classifyWaterfallFrame(const QByteArray& frame,
                                        int zoomCap = 14);
void resetSoundAdpcmState(SoundAdpcmState* state);
void invalidateSoundAdpcmState(SoundAdpcmState* state);
SoundFrameDecodeResult decodeSoundFrame(const QByteArray& frame,
                                        SoundAdpcmState* adpcmState = nullptr);
WaterfallFrameDecodeResult decodeWaterfallFrame(const QByteArray& frame,
                                                int zoomCap = 14);
ReceiverMetadata parseStatusPayload(const QByteArray& payload,
                                    const QString& serverHeader = {});
bool mergeReceiverMetadata(ReceiverMetadata* target,
                           const ReceiverMetadata& source);
bool updateReceiverMetadataFromMsgToken(const MsgToken& token,
                                        ReceiverMetadata* metadata);
int squelchSliderLevelToMarginDb(int level);
QString formatSquelchCommand(bool enabled, int thresholdDb,
                             double tailSeconds = kSquelchDefaultTailSeconds);
int agcDecayMsForMode(const QString& mode);
QString formatAgcCommand(bool enabled, bool hang, int thresholdDb,
                         int manualGainDb, int decayMs);
MeterReading meterUnavailable(MeterSource source, const QString& notes = {});
MeterReading extractMeterFromSndVerifiedLayout(const QByteArray& frame,
                                               const MeterContext& context);
MeterReading computeRelativeAudioLevel(const float* samples, int sampleCount);
MeterReading computeRelativeWaterfallLevel(const QVector<float>& bins);
QString convertDbmToSUnits(float dbm);

} // namespace AetherSDR::KiwiSdrProtocol

Q_DECLARE_METATYPE(AetherSDR::KiwiSdrProtocol::MeterReading)
Q_DECLARE_METATYPE(AetherSDR::KiwiSdrProtocol::ReceiverMetadata)
Q_DECLARE_METATYPE(AetherSDR::KiwiSdrProtocol::ProtocolState)
