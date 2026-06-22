#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>
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
float waterfallByteToDisplayLevel(unsigned char value);
WaterfallAperture autoWaterfallAperture(const QVector<float>& binsDbm);
float waterfallColorIndex(float dbm, float minDbm, float maxDbm);
QVector<MsgToken> parseMsgTokens(const QString& message);
IpLimitNotice parseIpLimitNotice(const QString& valueText);
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
