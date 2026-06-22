#include "KiwiSdrProtocol.h"

#include <QDateTime>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace AetherSDR::KiwiSdrProtocol {
namespace {

constexpr int kObservedExtendedSoundFrameBytes = 1034;
constexpr int kServerSoundHeaderBytes = 10;
constexpr int kObservedMeterOffset = 8;
constexpr float kSndMeterDbmOffset = -127.0f;
constexpr float kSndMeterDbPerRawUnit = 0.1f;
constexpr quint8 kSoundSquelchFlag = 0x40;

int normalizeEnabledSquelchMarginDb(int thresholdDb)
{
    const int clamped = std::clamp(thresholdDb,
                                   kSquelchServerMinMarginDb,
                                   kSquelchServerMaxMarginDb);
    return clamped == kSquelchOffLevel ? 1 : clamped;
}

float percentile(QVector<float> values, float fraction)
{
    if (values.isEmpty()) {
        return 0.0f;
    }

    const int lastIndex = values.size() - 1;
    const int index = std::clamp(
        static_cast<int>(std::lround(std::clamp(fraction, 0.0f, 1.0f)
            * static_cast<float>(lastIndex))),
        0,
        lastIndex);
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}

qint16 readSignedBigEndian16(const QByteArray& bytes, int offset)
{
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    const quint16 value = (static_cast<quint16>(data[0]) << 8)
        | static_cast<quint16>(data[1]);
    return static_cast<qint16>(value);
}

} // namespace

SoundFrameHeader parseSoundFrameHeader(const QByteArray& frame)
{
    if (frame.size() < 6 || !frame.startsWith("SND")) {
        return {};
    }

    SoundFrameHeader header;
    header.flags = static_cast<uchar>(frame[3]);
    header.squelched = (header.flags & kSoundSquelchFlag) != 0;
    if (frame.size() >= kServerSoundHeaderBytes) {
        const auto* data = reinterpret_cast<const uchar*>(frame.constData() + 4);
        const quint32 counter = static_cast<quint32>(data[0])
            | (static_cast<quint32>(data[1]) << 8)
            | (static_cast<quint32>(data[2]) << 16)
            | (static_cast<quint32>(data[3]) << 24);
        header.sequence = static_cast<int>(counter & 0xffu);
    } else {
        header.sequence = header.flags;
    }
    header.valid = true;
    return header;
}

WaterfallLineHeader parseWaterfallLineHeader(const QByteArray& frame)
{
    if (frame.size() < 4 || !frame.startsWith("W/F")) {
        return {};
    }

    WaterfallLineHeader header;
    header.sequence = static_cast<uchar>(frame[3]);
    header.valid = true;
    return header;
}

quint64 sequenceGapCount(int previousSequence, int currentSequence)
{
    if (previousSequence < 0 || currentSequence < 0) {
        return 0;
    }

    const int previous = previousSequence & 0xff;
    const int current = currentSequence & 0xff;
    if (previous == current) {
        return 0;
    }

    const int delta = (current - previous + 256) % 256;
    return delta > 0 ? static_cast<quint64>(delta - 1) : 0;
}

float waterfallByteToDisplayLevel(unsigned char value)
{
    // Kiwi server W/F direct rows encode negative dB by decrementing the
    // clamped bin dB and wrapping it into an unsigned byte. So 255 means
    // 0 dB, 155 means -100 dB, and 55 means -200 dB.
    return std::clamp(static_cast<float>(value) - 255.0f, -200.0f, 0.0f);
}

WaterfallAperture autoWaterfallAperture(const QVector<float>& binsDbm)
{
    QVector<float> finite;
    finite.reserve(binsDbm.size());
    for (float dbm : binsDbm) {
        if (std::isfinite(dbm)) {
            finite.append(dbm);
        }
    }

    if (finite.size() < 8) {
        return {};
    }

    const float noiseFloor = percentile(finite, 0.50f);
    const float signalPeak = percentile(finite, 0.98f);
    WaterfallAperture aperture;
    aperture.minDbm = noiseFloor - 10.0f;
    aperture.maxDbm = signalPeak + 30.0f;
    if (aperture.maxDbm <= aperture.minDbm + 1.0f) {
        aperture.maxDbm = aperture.minDbm + 1.0f;
    }
    aperture.valid = true;
    return aperture;
}

float waterfallColorIndex(float dbm, float minDbm, float maxDbm)
{
    const float span = std::max(1.0f, maxDbm - minDbm);
    const float normalized = std::clamp((dbm - minDbm) / span, 0.0f, 1.0f);
    return std::sqrt(normalized);
}

QVector<MsgToken> parseMsgTokens(const QString& message)
{
    const QString body = message.startsWith(QStringLiteral("MSG"))
        ? message.mid(3).trimmed()
        : message.trimmed();
    const QStringList parts =
        body.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    QVector<MsgToken> tokens;
    tokens.reserve(parts.size());
    for (const QString& part : parts) {
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq == 0) {
            continue;
        }

        MsgToken token;
        if (eq > 0) {
            token.key = part.left(eq);
            token.value = part.mid(eq + 1);
            token.hasValue = true;
        } else {
            token.key = part;
            token.hasValue = false;
        }
        tokens.append(token);
    }
    return tokens;
}

IpLimitNotice parseIpLimitNotice(const QString& valueText)
{
    const QString decoded =
        QUrl::fromPercentEncoding(valueText.toUtf8()).trimmed();
    const int comma = decoded.indexOf(QLatin1Char(','));
    const QString minutesText =
        comma >= 0 ? decoded.left(comma).trimmed() : decoded;

    bool minutesOk = false;
    const int minutes = minutesText.toInt(&minutesOk);
    if (!minutesOk || minutes <= 0) {
        return {};
    }

    IpLimitNotice notice;
    notice.minutes = minutes;
    notice.address = comma >= 0 ? decoded.mid(comma + 1).trimmed() : QString();
    notice.valid = true;
    return notice;
}

QString formatSquelchCommand(bool enabled, int thresholdDb,
                             double tailSeconds)
{
    const int threshold = enabled
        ? normalizeEnabledSquelchMarginDb(thresholdDb)
        : kSquelchOffLevel;
    const double tail = std::clamp(tailSeconds, 0.0, 2.0);
    return QStringLiteral("SET squelch=%1 param=%2")
        .arg(threshold)
        .arg(tail, 0, 'f', 2);
}

int squelchSliderLevelToMarginDb(int level)
{
    static_assert(kSquelchUiMaxLevel % 2 == 1);
    const int clamped = std::clamp(level, kSquelchUiMinLevel,
                                   kSquelchUiMaxLevel);
    const int margin = (clamped * 2) - kSquelchUiMaxLevel;
    return std::clamp(margin,
                      kSquelchServerMinMarginDb,
                      kSquelchServerMaxMarginDb);
}

int agcDecayMsForMode(const QString& mode)
{
    const QString normalized = mode.trimmed().toLower();
    if (normalized == QStringLiteral("fast")) {
        return kAgcFastDecayMs;
    }
    if (normalized == QStringLiteral("slow")) {
        return kAgcSlowDecayMs;
    }
    return kAgcMedDecayMs;
}

QString formatAgcCommand(bool enabled, bool hang, int thresholdDb,
                         int manualGainDb, int decayMs)
{
    const int clampedThreshold = std::clamp(
        thresholdDb, kAgcThresholdMinDb, kAgcThresholdMaxDb);
    const int clampedManualGain = std::clamp(
        manualGainDb, kAgcManualGainMinDb, kAgcManualGainMaxDb);
    const int clampedDecay = std::clamp(
        decayMs, kAgcDecayMinMs, kAgcDecayMaxMs);
    return QStringLiteral(
        "SET agc=%1 hang=%2 thresh=%3 slope=%4 decay=%5 manGain=%6")
        .arg(enabled ? 1 : 0)
        .arg(hang ? 1 : 0)
        .arg(clampedThreshold)
        .arg(kAgcSlopeDb)
        .arg(clampedDecay)
        .arg(clampedManualGain);
}

MeterReading meterUnavailable(MeterSource source, const QString& notes)
{
    MeterReading reading;
    reading.timestampUtcMs = QDateTime::currentMSecsSinceEpoch();
    reading.source = source;
    reading.capability = MeterCapability::Unavailable;
    reading.valid = false;
    reading.confidence = MeterConfidence::None;
    reading.label = QStringLiteral("Meter unavailable");
    reading.notes = notes;
    return reading;
}

MeterReading extractMeterFromSndVerifiedLayout(const QByteArray& frame,
                                               const MeterContext& context)
{
    Q_UNUSED(context);
    if (frame.size() == kObservedExtendedSoundFrameBytes
        && frame.startsWith("SND")) {
        const qint16 rawMeter =
            readSignedBigEndian16(frame, kObservedMeterOffset);
        const float dbm = kSndMeterDbmOffset
            + (static_cast<float>(rawMeter)
               * kSndMeterDbPerRawUnit);

        MeterReading reading;
        reading.timestampUtcMs = QDateTime::currentMSecsSinceEpoch();
        reading.source = MeterSource::SndMetadata;
        reading.capability = MeterCapability::CalibratedSndMeter;
        reading.rawValue = static_cast<float>(rawMeter);
        reading.hasRawValue = true;
        reading.dbm = dbm;
        reading.hasDbm = true;
        reading.sUnits = convertDbmToSUnits(dbm);
        reading.valid = true;
        reading.confidence = MeterConfidence::Verified;
        reading.label = QStringLiteral("S-Meter");
        reading.notes = QStringLiteral(
            "SND bytes 8-9 big-endian server S-meter, dbm=raw/10-127");
        return reading;
    }

    return meterUnavailable(
        MeterSource::SndMetadata,
        QStringLiteral("SND meter layout not verified"));
}

MeterReading computeRelativeAudioLevel(const float* samples, int sampleCount)
{
    if (!samples || sampleCount <= 0) {
        return meterUnavailable(
            MeterSource::AudioSamples,
            QStringLiteral("No decoded audio samples available"));
    }

    double sumSquares = 0.0;
    int validSamples = 0;
    for (int i = 0; i < sampleCount; ++i) {
        const float sample = std::clamp(samples[i], -1.0f, 1.0f);
        if (!std::isfinite(sample)) {
            continue;
        }
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
        ++validSamples;
    }

    if (validSamples <= 0) {
        return meterUnavailable(
            MeterSource::AudioSamples,
            QStringLiteral("Decoded audio samples were not finite"));
    }

    const double rms = std::sqrt(sumSquares / static_cast<double>(validSamples));
    constexpr float kMinimumDbfs = -60.0f;
    constexpr float kMaximumDbfs = 0.0f;
    const float dbfs = rms > 0.0
        ? static_cast<float>(20.0 * std::log10(rms))
        : kMinimumDbfs;
    const float relativeLevel = std::clamp(
        (dbfs - kMinimumDbfs) / (kMaximumDbfs - kMinimumDbfs),
        0.0f,
        1.0f);

    MeterReading reading;
    reading.timestampUtcMs = QDateTime::currentMSecsSinceEpoch();
    reading.source = MeterSource::AudioSamples;
    reading.capability = MeterCapability::RelativeAudio;
    reading.relativeLevel = relativeLevel;
    reading.hasRelativeLevel = true;
    reading.valid = true;
    reading.confidence = MeterConfidence::Medium;
    reading.label = QStringLiteral("Audio level, relative");
    return reading;
}

MeterReading computeRelativeWaterfallLevel(const QVector<float>& bins)
{
    QVector<float> finite;
    finite.reserve(bins.size());
    for (float bin : bins) {
        if (std::isfinite(bin)) {
            finite.append(bin);
        }
    }

    if (finite.isEmpty()) {
        return meterUnavailable(
            MeterSource::WaterfallBins,
            QStringLiteral("No decoded waterfall bins available"));
    }

    const float signal = percentile(finite, 0.95f);
    constexpr float kDisplayFloor = -200.0f;
    constexpr float kDisplayCeiling = 0.0f;
    const float relativeLevel = std::clamp(
        (signal - kDisplayFloor) / (kDisplayCeiling - kDisplayFloor),
        0.0f,
        1.0f);

    MeterReading reading;
    reading.timestampUtcMs = QDateTime::currentMSecsSinceEpoch();
    reading.source = MeterSource::WaterfallBins;
    reading.capability = MeterCapability::RelativeWaterfall;
    reading.relativeLevel = relativeLevel;
    reading.hasRelativeLevel = true;
    reading.valid = true;
    reading.confidence = MeterConfidence::Low;
    reading.label = QStringLiteral("Waterfall intensity, relative");
    reading.notes = QStringLiteral("Relative waterfall value, not calibrated dBm");
    return reading;
}

QString convertDbmToSUnits(float dbm)
{
    constexpr float kS9Dbm = -73.0f;
    constexpr float kDbPerSUnit = 6.0f;
    if (dbm > kS9Dbm) {
        return QStringLiteral("S9 + %1 dB")
            .arg(qRound(dbm - kS9Dbm));
    }

    const int sValue = std::clamp(
        qRound(9.0f + ((dbm - kS9Dbm) / kDbPerSUnit)),
        0,
        9);
    return QStringLiteral("S%1").arg(sValue);
}

} // namespace AetherSDR::KiwiSdrProtocol
