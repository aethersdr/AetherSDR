#include "KiwiSdrProtocol.h"

#include <QDateTime>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace AetherSDR::KiwiSdrProtocol {
namespace {

constexpr int kObservedExtendedSoundFrameBytes = 1034;
constexpr int kServerSoundHeaderBytes = 10;
constexpr int kSpecSoundHeaderBytes = 6;
constexpr quint8 kSoundStereoFlag = 0x08;
constexpr quint8 kSoundCompressedFlag = 0x10;
constexpr quint8 kSoundRestartFlag = 0x20;
constexpr int kSpecWaterfallHeaderBytes = 4;
constexpr int kExtendedWaterfallHeaderBytes = 16;
constexpr int kDefaultWaterfallFftBins = 1024;
constexpr int kWaterfallAutoNoiseIndex = 512;
constexpr int kWaterfallAutoSignalIndex = 1003;
constexpr int kZoomedWaterfallPrefixBytes = 5;
constexpr int kWaterfallAdpcmPadSamples = 10;
constexpr int kWaterfallCompactPayloadBytes =
    (kWaterfallAdpcmPadSamples + kDefaultWaterfallFftBins) / 2;
constexpr quint32 kWaterfallZoomMask = 0x0000ffffu;
constexpr quint32 kWaterfallCompressionFlag = 0x00010000u;
constexpr int kObservedMeterOffset = 8;
constexpr float kSndMeterDbmOffset = -127.0f;
constexpr float kSndMeterDbPerRawUnit = 0.1f;
constexpr float kDefaultWaterfallMinDbm = -110.0f;
constexpr float kDefaultWaterfallMaxDbm = -10.0f;
constexpr float kWaterfallZoomCorrectionDb = 3.0f;
constexpr quint8 kSoundSquelchFlag = 0x40;
constexpr quint8 kSoundLittleEndianFlag = 0x80;
constexpr qsizetype kMaxStableStatusValueChars = 120;
constexpr int kMaxStableStatusFields = 16;

const int kImaAdpcmIndexAdjust[16] = {
    -1, -1, -1, -1,
    2, 4, 6, 8,
    -1, -1, -1, -1,
    2, 4, 6, 8,
};

const int kImaAdpcmStepSize[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
    1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767,
};

struct WaterfallFrameShape {
    bool valid{false};
    bool extendedHeader{false};
    bool compressed{false};
    int payloadOffset{kSpecWaterfallHeaderBytes};
    int payloadBytes{0};
    int zoom{0};
};

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

QVector<float> averageWaterfallRows(const QVector<QVector<float>>& rowsDbm)
{
    QVector<float> averaged(kDefaultWaterfallFftBins, 0.0f);
    int validRows = 0;
    for (const QVector<float>& row : rowsDbm) {
        if (row.size() != kDefaultWaterfallFftBins) {
            continue;
        }

        bool finite = true;
        for (float dbm : row) {
            if (!std::isfinite(dbm)) {
                finite = false;
                break;
            }
        }
        if (!finite) {
            continue;
        }

        for (int i = 0; i < row.size(); ++i) {
            averaged[i] += row[i];
        }
        ++validRows;
    }

    if (validRows == 0) {
        return {};
    }

    const float invRows = 1.0f / static_cast<float>(validRows);
    for (float& dbm : averaged) {
        dbm *= invRows;
    }
    return averaged;
}

QVector<float> spatiallySmoothedWaterfallRow(const QVector<float>& binsDbm)
{
    if (binsDbm.size() != kDefaultWaterfallFftBins) {
        return {};
    }

    QVector<float> smoothed;
    smoothed.resize(binsDbm.size());
    const int binCount = static_cast<int>(binsDbm.size());
    for (int i = 0; i < binCount; ++i) {
        const int first = std::max(0, i - 1);
        const int last = std::min(binCount - 1, i + 1);
        float sum = 0.0f;
        int count = 0;
        for (int j = first; j <= last; ++j) {
            sum += binsDbm[j];
            ++count;
        }
        smoothed[i] = sum / static_cast<float>(count);
    }
    return smoothed;
}

qint16 readSignedBigEndian16(const QByteArray& bytes, int offset)
{
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    const quint16 value = (static_cast<quint16>(data[0]) << 8)
        | static_cast<quint16>(data[1]);
    return static_cast<qint16>(value);
}

quint32 readLittleEndianU32(const char* data)
{
    const auto* bytes = reinterpret_cast<const uchar*>(data);
    return static_cast<quint32>(bytes[0])
        | (static_cast<quint32>(bytes[1]) << 8)
        | (static_cast<quint32>(bytes[2]) << 16)
        | (static_cast<quint32>(bytes[3]) << 24);
}

int soundPayloadOffset(const QByteArray& frame)
{
    return frame.size() >= kServerSoundHeaderBytes
        ? kServerSoundHeaderBytes
        : kSpecSoundHeaderBytes;
}

bool soundFrameCompressed(quint8 flags)
{
    return (flags & kSoundCompressedFlag) != 0;
}

bool soundFrameStereo(quint8 flags)
{
    return (flags & kSoundStereoFlag) != 0;
}

bool soundFrameRestart(quint8 flags)
{
    return (flags & kSoundRestartFlag) != 0;
}

bool soundFrameLittleEndian(quint8 flags)
{
    return (flags & kSoundLittleEndianFlag) != 0;
}

WaterfallFrameShape inspectWaterfallFrame(const QByteArray& frame, int zoomCap)
{
    if (!frame.startsWith("W/F")
        || frame.size() <= kSpecWaterfallHeaderBytes) {
        return {};
    }

    WaterfallFrameShape shape;
    shape.valid = true;
    if (frame.size() >= kExtendedWaterfallHeaderBytes
        && frame.size() != kSpecWaterfallHeaderBytes + kDefaultWaterfallFftBins) {
        const quint32 flagsAndZoom =
            readLittleEndianU32(frame.constData() + 8);
        const int parsedZoom =
            static_cast<int>(flagsAndZoom & kWaterfallZoomMask);
        if (parsedZoom >= 0 && parsedZoom <= std::clamp(zoomCap, 0, 30)) {
            shape.extendedHeader = true;
            shape.zoom = parsedZoom;
            shape.compressed =
                (flagsAndZoom & kWaterfallCompressionFlag) != 0;
            shape.payloadOffset = kExtendedWaterfallHeaderBytes;
        }
    }
    shape.payloadBytes = static_cast<int>(
        std::max<qsizetype>(0, frame.size() - shape.payloadOffset));
    return shape;
}

quint8 decodeImaAdpcmU8Nibble(quint8 code, int* predictor, int* index)
{
    const int step = kImaAdpcmStepSize[std::clamp(*index, 0, 88)];
    int difference = step >> 3;
    if ((code & 0x01) != 0) {
        difference += step >> 2;
    }
    if ((code & 0x02) != 0) {
        difference += step >> 1;
    }
    if ((code & 0x04) != 0) {
        difference += step;
    }
    if ((code & 0x08) != 0) {
        difference = -difference;
    }

    *predictor = std::clamp(*predictor + difference, 0, 255);
    *index = std::clamp(*index + kImaAdpcmIndexAdjust[code & 0x0f], 0, 88);
    return static_cast<quint8>(*predictor);
}

qint16 decodeImaAdpcmI16Nibble(quint8 code, SoundAdpcmState* state)
{
    const int step = kImaAdpcmStepSize[std::clamp(state->index, 0, 88)];
    int difference = step >> 3;
    if ((code & 0x01) != 0) {
        difference += step >> 2;
    }
    if ((code & 0x02) != 0) {
        difference += step >> 1;
    }
    if ((code & 0x04) != 0) {
        difference += step;
    }
    if ((code & 0x08) != 0) {
        difference = -difference;
    }

    state->predictor = std::clamp(state->predictor + difference,
                                  -32768,
                                  32767);
    state->index = std::clamp(state->index
                                  + kImaAdpcmIndexAdjust[code & 0x0f],
                              0,
                              88);
    return static_cast<qint16>(state->predictor);
}

QVector<quint8> decodeWaterfallAdpcmU8(const uchar* payload, int payloadBytes)
{
    QVector<quint8> decoded;
    decoded.reserve(payloadBytes * 2);
    int predictor = 0;
    int index = 0;
    for (int i = 0; i < payloadBytes; ++i) {
        const quint8 packed = payload[i];
        decoded.append(decodeImaAdpcmU8Nibble(packed & 0x0f,
                                              &predictor,
                                              &index));
        decoded.append(decodeImaAdpcmU8Nibble((packed >> 4) & 0x0f,
                                              &predictor,
                                              &index));
    }
    return decoded;
}

float pcm16ToFloat(qint16 sample)
{
    return std::clamp(static_cast<float>(sample) / 32768.0f, -1.0f, 1.0f);
}

double metadataValueToMhz(double value)
{
    if (!std::isfinite(value) || value <= 0.0) {
        return 0.0;
    }
    if (value >= 1000000.0) {
        return value / 1000000.0;
    }
    if (value >= 1000.0) {
        return value / 1000.0;
    }
    return value;
}

bool appendUniqueStatusField(QStringList* fields, const QString& key,
                             const QString& value)
{
    if (!fields || key.isEmpty()) {
        return false;
    }

    QString clippedValue = value.trimmed();
    if (clippedValue.size() > kMaxStableStatusValueChars) {
        clippedValue = clippedValue.left(kMaxStableStatusValueChars)
            + QStringLiteral("...");
    }
    const QString entry = clippedValue.isEmpty()
        ? key
        : QStringLiteral("%1=%2").arg(key, clippedValue);
    if (!fields->contains(entry)) {
        fields->append(entry);
        return true;
    }
    return false;
}

void trimStableStatusFields(QStringList* fields)
{
    if (!fields) {
        return;
    }
    while (fields->size() > kMaxStableStatusFields) {
        fields->removeFirst();
    }
}

bool shouldRetainUnstructuredStatusField(const QString& key, bool hasValue)
{
    if (!hasValue) {
        return true;
    }

    return key != QStringLiteral("audio_init")
        && key != QStringLiteral("audio_rate")
        && key != QStringLiteral("sample_rate")
        && key != QStringLiteral("wf_fps")
        && key != QStringLiteral("zoom")
        && key != QStringLiteral("start")
        && key != QStringLiteral("cf")
        && key != QStringLiteral("maxdb")
        && key != QStringLiteral("mindb")
        && key != QStringLiteral("wf_speed")
        && key != QStringLiteral("interp")
        && key != QStringLiteral("window_func")
        && key != QStringLiteral("send_dB")
        && key != QStringLiteral("wf_fft_size")
        && key != QStringLiteral("zoom_cap")
        && key != QStringLiteral("zoom_max");
}

bool setIntField(int* target, bool* hasTarget, int value)
{
    if (!target || !hasTarget) {
        return false;
    }
    const bool changed = !*hasTarget || *target != value;
    *target = value;
    *hasTarget = true;
    return changed;
}

bool setBoolField(bool* target, bool* hasTarget, bool value)
{
    if (!target || !hasTarget) {
        return false;
    }
    const bool changed = !*hasTarget || *target != value;
    *target = value;
    *hasTarget = true;
    return changed;
}

bool setCampStatusField(CampStatus* target, bool* hasTarget,
                        CampStatus value)
{
    if (!target || !hasTarget) {
        return false;
    }
    const bool changed = !*hasTarget || *target != value;
    *target = value;
    *hasTarget = true;
    return changed;
}

bool setDoubleField(double* target, bool* hasTarget, double value,
                    double epsilon = 0.001)
{
    if (!target || !hasTarget || !std::isfinite(value)) {
        return false;
    }
    const bool changed = !*hasTarget || std::abs(*target - value) > epsilon;
    *target = value;
    *hasTarget = true;
    return changed;
}

QString parseKiwiVersionFromServerHeader(const QString& serverHeader)
{
    const QString marker = QStringLiteral("KiwiSDR_");
    const int markerIndex = serverHeader.indexOf(marker, 0, Qt::CaseInsensitive);
    if (markerIndex < 0) {
        return QString();
    }
    const int valueStart = markerIndex + marker.size();
    int valueEnd = serverHeader.indexOf(QLatin1Char('/'), valueStart);
    if (valueEnd < 0) {
        valueEnd = serverHeader.indexOf(QLatin1Char(' '), valueStart);
    }
    if (valueEnd < 0) {
        valueEnd = serverHeader.size();
    }
    return serverHeader.mid(valueStart, valueEnd - valueStart).trimmed();
}

bool mergeString(QString* target, const QString& source)
{
    if (!target || source.trimmed().isEmpty()) {
        return false;
    }
    const QString trimmed = source.trimmed();
    if (*target == trimmed) {
        return false;
    }
    *target = trimmed;
    return true;
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

float waterfallByteToDbm(unsigned char value)
{
    return static_cast<float>(value) - 255.0f;
}

float waterfallByteToDisplayLevel(unsigned char value)
{
    // Kiwi W/F rows encode dBm-like negative levels as unsigned bytes:
    // 255 means 0 dB, 155 means -100 dB, and 0 means -255 dB.
    return waterfallByteToDbm(value);
}

float calibratedWaterfallLevel(float dbm, int calibrationDb)
{
    return std::isfinite(dbm)
        ? dbm + static_cast<float>(calibrationDb)
        : dbm;
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

WaterfallDisplayRange autoWaterfallDisplayRange(const QVector<float>& binsDbm)
{
    if (binsDbm.size() != kDefaultWaterfallFftBins) {
        return {};
    }

    QVector<float> sorted = binsDbm;
    for (float dbm : sorted) {
        if (!std::isfinite(dbm)) {
            return {};
        }
    }
    std::sort(sorted.begin(), sorted.end());

    WaterfallDisplayRange range;
    range.minDbm = sorted[kWaterfallAutoNoiseIndex] - 10.0f;
    range.maxDbm = sorted[kWaterfallAutoSignalIndex] + 30.0f;
    if (range.maxDbm <= range.minDbm + 1.0f) {
        range.maxDbm = range.minDbm + 1.0f;
    }
    range.valid = true;
    return range;
}

WaterfallDisplayRange autoWaterfallDisplayRangeFromRows(
    const QVector<QVector<float>>& rowsDbm)
{
    const QVector<float> averaged = averageWaterfallRows(rowsDbm);
    if (averaged.isEmpty()) {
        return {};
    }

    const QVector<float> smoothed =
        spatiallySmoothedWaterfallRow(averaged);
    if (smoothed.isEmpty()) {
        return {};
    }

    return autoWaterfallDisplayRange(smoothed);
}

float waterfallZoomCorrectionDb(int zoom)
{
    return kWaterfallZoomCorrectionDb
        * static_cast<float>(std::clamp(zoom, 0, 64));
}

WaterfallDisplayRange adjustedWaterfallDisplayRange(float minDbm,
                                                    float maxDbm,
                                                    int ceilingOffsetDb,
                                                    int floorOffsetDb)
{
    if (!std::isfinite(minDbm) || !std::isfinite(maxDbm)) {
        return {};
    }

    WaterfallDisplayRange range;
    range.minDbm = minDbm + static_cast<float>(floorOffsetDb);
    range.maxDbm = maxDbm + static_cast<float>(ceilingOffsetDb);
    if (range.maxDbm <= range.minDbm + 1.0f) {
        range.maxDbm = range.minDbm + 1.0f;
    }
    range.valid = true;
    return range;
}

WaterfallDisplayRange zoomAdjustedWaterfallDisplayRange(float minDbm,
                                                        float maxDbm,
                                                        int sourceZoom,
                                                        int currentZoom)
{
    const float sourceCorrection = waterfallZoomCorrectionDb(sourceZoom);
    const float currentCorrection = waterfallZoomCorrectionDb(currentZoom);
    return adjustedWaterfallDisplayRange(
        minDbm,
        maxDbm,
        0,
        static_cast<int>(std::lround(sourceCorrection - currentCorrection)));
}

WaterfallDisplayRange defaultWaterfallDisplayRange(int zoom,
                                                   int ceilingOffsetDb,
                                                   int floorOffsetDb)
{
    return adjustedWaterfallDisplayRange(
        kDefaultWaterfallMinDbm - waterfallZoomCorrectionDb(zoom),
        kDefaultWaterfallMaxDbm,
        ceilingOffsetDb,
        floorOffsetDb);
}

float waterfallColorIndex(float dbm, float minDbm, float maxDbm)
{
    const float span = std::max(1.0f, maxDbm - minDbm);
    return std::clamp((dbm - minDbm) / span, 0.0f, 1.0f);
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

ApiPolicy apiPolicyFor(int extApi, int usersMax)
{
    if (extApi < 0) {
        return ApiPolicy::Unknown;
    }
    if (extApi == 0) {
        return ApiPolicy::Disabled;
    }
    if (usersMax <= 0) {
        return ApiPolicy::Unknown;
    }
    if (usersMax > 0 && extApi < usersMax) {
        return ApiPolicy::Limited;
    }
    return ApiPolicy::Open;
}

QString apiPolicyName(ApiPolicy policy)
{
    switch (policy) {
    case ApiPolicy::Unknown:
        return QStringLiteral("unknown");
    case ApiPolicy::Disabled:
        return QStringLiteral("disabled");
    case ApiPolicy::Limited:
        return QStringLiteral("limited");
    case ApiPolicy::Open:
        return QStringLiteral("open");
    }
    return QStringLiteral("unknown");
}

QString campStatusName(CampStatus status)
{
    switch (status) {
    case CampStatus::Unknown:
        return QStringLiteral("unknown");
    case CampStatus::Offered:
        return QStringLiteral("offered");
    case CampStatus::Queued:
        return QStringLiteral("queued");
    case CampStatus::Accepted:
        return QStringLiteral("accepted");
    case CampStatus::Rejected:
        return QStringLiteral("rejected");
    case CampStatus::AudioStopped:
        return QStringLiteral("audio-stopped");
    case CampStatus::Disconnected:
        return QStringLiteral("disconnected");
    }
    return QStringLiteral("unknown");
}

QString authModeName(AuthMode mode)
{
    switch (mode) {
    case AuthMode::Unknown:
        return QStringLiteral("unknown");
    case AuthMode::PublicPasswordless:
        return QStringLiteral("public");
    case AuthMode::PasswordRequired:
        return QStringLiteral("password-required");
    case AuthMode::Rejected:
        return QStringLiteral("rejected");
    }
    return QStringLiteral("unknown");
}

QString streamModeName(StreamMode mode)
{
    switch (mode) {
    case StreamMode::Unknown:
        return QStringLiteral("unknown");
    case StreamMode::Sound:
        return QStringLiteral("SND");
    case StreamMode::Waterfall:
        return QStringLiteral("W/F");
    case StreamMode::Extension:
        return QStringLiteral("EXT");
    case StreamMode::Iq:
        return QStringLiteral("IQ");
    case StreamMode::Gnss:
        return QStringLiteral("GNSS");
    }
    return QStringLiteral("unknown");
}

QString frameLayoutName(FrameLayout layout)
{
    switch (layout) {
    case FrameLayout::Unknown:
        return QStringLiteral("unknown");
    case FrameLayout::SndPcm16:
        return QStringLiteral("snd-pcm16");
    case FrameLayout::SndObservedPcm16WithMeter:
        return QStringLiteral("snd-observed-pcm16-meter");
    case FrameLayout::SndCompressed:
        return QStringLiteral("snd-compressed-adpcm16");
    case FrameLayout::WaterfallDirectBins:
        return QStringLiteral("wf-direct-bins");
    case FrameLayout::WaterfallCompactEncoded:
        return QStringLiteral("wf-compact-encoded");
    case FrameLayout::Extension:
        return QStringLiteral("ext");
    case FrameLayout::UnknownBinary:
        return QStringLiteral("unknown-binary");
    case FrameLayout::Unsupported:
        return QStringLiteral("unsupported");
    }
    return QStringLiteral("unknown");
}

ProtocolState defaultProtocolState()
{
    ProtocolState state;
    state.sound.mode = StreamMode::Sound;
    state.sound.requested = true;
    state.sound.uncompressedRequested = true;
    state.sound.supportedLayouts = {
        FrameLayout::SndPcm16,
        FrameLayout::SndObservedPcm16WithMeter,
        FrameLayout::SndCompressed,
    };
    state.waterfall.mode = StreamMode::Waterfall;
    state.waterfall.requested = true;
    state.waterfall.uncompressedRequested = true;
    state.waterfall.supportedLayouts = {
        FrameLayout::WaterfallDirectBins,
        FrameLayout::WaterfallCompactEncoded,
    };
    state.unsupportedFeatureReasons = {
        QStringLiteral("IQ, GNSS, and Kiwi extensions are not requested by this receive-only client"),
    };
    return state;
}

bool diagnosticCompressionFlagEnabled(const QByteArray& value)
{
    const QString normalized =
        QString::fromLocal8Bit(value).trimmed().toLower();
    return normalized == QStringLiteral("1")
        || normalized == QStringLiteral("true")
        || normalized == QStringLiteral("yes")
        || normalized == QStringLiteral("on");
}

bool diagnosticWaterfallCompressionFlagEnabled(const QByteArray& value)
{
    return diagnosticCompressionFlagEnabled(value);
}

QString formatAuthCommand(const QString& password)
{
    // The LGPL Kiwi server parses p= as a whitespace-delimited token capped at
    // 256 encoded characters, maps a bare '#' to no password, then URL-decodes
    // the token before comparison (rx/rx_cmd.cpp and support/str.cpp, server
    // revision 417e2c8). A real '#' must therefore become %23.
    const QByteArray encodedBytes = password.isEmpty()
        ? QByteArrayLiteral("#")
        : QUrl::toPercentEncoding(password);
    if (encodedBytes.size() > kAuthPasswordEncodedMaxLength) {
        return QString();
    }
    const QString encoded = QString::fromLatin1(encodedBytes);
    return QStringLiteral("SET auth t=kiwi p=%1").arg(encoded);
}

bool authPasswordFitsServerLimit(const QString& password)
{
    if (password.isEmpty()) {
        return true;
    }
    return QUrl::toPercentEncoding(password).size()
        <= kAuthPasswordEncodedMaxLength;
}

QString formatSoundCompressionCommand(bool compressed)
{
    return QStringLiteral("SET compression=%1").arg(compressed ? 1 : 0);
}

QString formatWaterfallCompressionCommand(bool compressed)
{
    return QStringLiteral("SET wf_comp=%1").arg(compressed ? 1 : 0);
}

FrameObservation classifySoundFrame(const QByteArray& frame)
{
    FrameObservation observation;
    observation.stream = StreamMode::Sound;
    observation.frameBytes = frame.size();
    if (!frame.startsWith("SND")) {
        observation.layout = FrameLayout::UnknownBinary;
        observation.unsupportedReason = QStringLiteral("Binary frame is not an SND frame");
        return observation;
    }

    const SoundFrameHeader header = parseSoundFrameHeader(frame);
    const int payloadOffset = header.valid ? soundPayloadOffset(frame) : 0;
    observation.payloadBytes = static_cast<int>(
        std::max<qsizetype>(0, frame.size() - payloadOffset));
    if (!header.valid) {
        observation.layout = FrameLayout::Unsupported;
        observation.unsupportedReason = QStringLiteral("SND header is too short");
        return observation;
    }
    if (soundFrameCompressed(header.flags)) {
        observation.layout = FrameLayout::SndCompressed;
        if (frame.size() < kServerSoundHeaderBytes) {
            observation.unsupportedReason =
                QStringLiteral("SND compressed ADPCM header is too short");
            return observation;
        }
        if (soundFrameStereo(header.flags)) {
            observation.unsupportedReason =
                QStringLiteral("SND compressed stereo/IQ layout is unsupported");
            return observation;
        }
        if (observation.payloadBytes <= 0) {
            observation.unsupportedReason =
                QStringLiteral("SND compressed ADPCM payload is empty");
            return observation;
        }
        observation.supported = true;
        return observation;
    }
    if (observation.payloadBytes <= 0 || (observation.payloadBytes % 2) != 0) {
        observation.layout = FrameLayout::Unsupported;
        observation.unsupportedReason = QStringLiteral("SND PCM payload is empty or not 16-bit aligned");
        return observation;
    }

    observation.supported = true;
    observation.layout = frame.size() == kObservedExtendedSoundFrameBytes
        ? FrameLayout::SndObservedPcm16WithMeter
        : FrameLayout::SndPcm16;
    return observation;
}

void resetSoundAdpcmState(SoundAdpcmState* state)
{
    if (state) {
        *state = SoundAdpcmState{};
    }
}

void invalidateSoundAdpcmState(SoundAdpcmState* state)
{
    if (state) {
        state->valid = false;
    }
}

SoundFrameDecodeResult decodeSoundFrame(const QByteArray& frame,
                                        SoundAdpcmState* adpcmState)
{
    SoundFrameDecodeResult result;
    result.observation = classifySoundFrame(frame);
    if (!result.observation.supported) {
        return result;
    }

    const SoundFrameHeader header = parseSoundFrameHeader(frame);
    if (!header.valid) {
        result.observation.supported = false;
        result.observation.layout = FrameLayout::Unsupported;
        result.observation.unsupportedReason =
            QStringLiteral("SND header is too short");
        return result;
    }

    const int payloadOffset = soundPayloadOffset(frame);
    const int payloadBytes = static_cast<int>(
        std::max<qsizetype>(0, frame.size() - payloadOffset));
    const auto* payload = reinterpret_cast<const uchar*>(
        frame.constData() + payloadOffset);

    if (result.observation.layout == FrameLayout::SndCompressed) {
        SoundAdpcmState localState;
        SoundAdpcmState* state = adpcmState ? adpcmState : &localState;
        if (soundFrameRestart(header.flags)) {
            resetSoundAdpcmState(state);
            result.decoderReset = true;
        }
        if (!state->valid) {
            result.observation.supported = false;
            result.observation.unsupportedReason = QStringLiteral(
                "SND compressed ADPCM decoder state is not synchronized");
            return result;
        }
        result.monoSamples.reserve(payloadBytes * 2);
        for (int i = 0; i < payloadBytes; ++i) {
            const quint8 packed = payload[i];
            result.monoSamples.append(
                pcm16ToFloat(decodeImaAdpcmI16Nibble(packed & 0x0f, state)));
            result.monoSamples.append(
                pcm16ToFloat(decodeImaAdpcmI16Nibble((packed >> 4) & 0x0f,
                                                     state)));
        }
        return result;
    }

    if ((payloadBytes % 2) != 0) {
        result.observation.supported = false;
        result.observation.layout = FrameLayout::Unsupported;
        result.observation.unsupportedReason =
            QStringLiteral("SND PCM payload is not 16-bit aligned");
        return result;
    }

    const int sampleCount = payloadBytes / 2;
    result.monoSamples.reserve(sampleCount);
    const bool littleEndian = soundFrameLittleEndian(header.flags);
    for (int i = 0; i < sampleCount; ++i) {
        int sample = 0;
        if (littleEndian) {
            sample = static_cast<int>(payload[2 * i])
                   | (static_cast<int>(payload[2 * i + 1]) << 8);
        } else {
            sample = (static_cast<int>(payload[2 * i]) << 8)
                   | static_cast<int>(payload[2 * i + 1]);
        }
        if ((sample & 0x8000) != 0) {
            sample -= 0x10000;
        }
        result.monoSamples.append(pcm16ToFloat(static_cast<qint16>(sample)));
    }
    return result;
}

FrameObservation classifyWaterfallFrame(const QByteArray& frame, int zoomCap)
{
    FrameObservation observation;
    observation.stream = StreamMode::Waterfall;
    observation.frameBytes = frame.size();
    if (!frame.startsWith("W/F")) {
        observation.layout = FrameLayout::UnknownBinary;
        observation.unsupportedReason = QStringLiteral("Binary frame is not a W/F frame");
        return observation;
    }
    if (frame.size() <= kSpecWaterfallHeaderBytes) {
        observation.layout = FrameLayout::Unsupported;
        observation.unsupportedReason = QStringLiteral("W/F frame has no row payload");
        return observation;
    }

    const WaterfallFrameShape shape = inspectWaterfallFrame(frame, zoomCap);
    observation.payloadBytes = shape.payloadBytes;
    const bool compactByShape =
        shape.extendedHeader
        && (shape.compressed
            || (shape.zoom > 0
                && shape.payloadBytes == kWaterfallCompactPayloadBytes));
    if (shape.extendedHeader
        && shape.zoom > 0
        && shape.payloadBytes == kZoomedWaterfallPrefixBytes) {
        observation.layout = FrameLayout::WaterfallCompactEncoded;
        observation.unsupportedReason =
            QStringLiteral("W/F compact row contains no decodable bins");
        return observation;
    }
    if (compactByShape) {
        observation.layout = FrameLayout::WaterfallCompactEncoded;
        if (shape.payloadBytes != kWaterfallCompactPayloadBytes) {
            observation.unsupportedReason =
                QStringLiteral("W/F compact row payload length is unsupported");
            return observation;
        }
        observation.supported = true;
        return observation;
    }
    if (shape.payloadBytes <= 0) {
        observation.layout = FrameLayout::Unsupported;
        observation.unsupportedReason = QStringLiteral("W/F frame has no row payload");
        return observation;
    }
    if (shape.payloadBytes != kDefaultWaterfallFftBins) {
        observation.layout = FrameLayout::Unsupported;
        observation.unsupportedReason =
            QStringLiteral("W/F direct row payload length is unsupported");
        return observation;
    }

    observation.layout = FrameLayout::WaterfallDirectBins;
    observation.supported = true;
    return observation;
}

WaterfallFrameDecodeResult decodeWaterfallFrame(const QByteArray& frame,
                                                int zoomCap)
{
    WaterfallFrameDecodeResult result;
    result.observation = classifyWaterfallFrame(frame, zoomCap);
    if (!result.observation.supported) {
        return result;
    }

    const WaterfallFrameShape shape = inspectWaterfallFrame(frame, zoomCap);
    if (!shape.valid || shape.payloadBytes <= 0) {
        result.binsDbm.clear();
        result.observation.supported = false;
        result.observation.layout = FrameLayout::Unsupported;
        result.observation.unsupportedReason =
            QStringLiteral("W/F frame has no row payload");
        return result;
    }

    result.binsDbm.reserve(kDefaultWaterfallFftBins);
    const auto* payload = reinterpret_cast<const uchar*>(
        frame.constData() + shape.payloadOffset);
    if (result.observation.layout == FrameLayout::WaterfallCompactEncoded) {
        const QVector<quint8> decoded =
            decodeWaterfallAdpcmU8(payload, shape.payloadBytes);
        if (decoded.size()
            != kWaterfallAdpcmPadSamples + kDefaultWaterfallFftBins) {
            result.binsDbm.clear();
            result.observation.supported = false;
            result.observation.unsupportedReason =
                QStringLiteral("W/F compact row decode length is unsupported");
            return result;
        }
        for (int i = kWaterfallAdpcmPadSamples; i < decoded.size(); ++i) {
            result.binsDbm.append(waterfallByteToDisplayLevel(decoded[i]));
        }
        return result;
    }

    for (int i = 0; i < shape.payloadBytes; ++i) {
        result.binsDbm.append(waterfallByteToDisplayLevel(payload[i]));
    }
    return result;
}

ReceiverMetadata parseStatusPayload(const QByteArray& payload,
                                    const QString& serverHeader)
{
    ReceiverMetadata metadata;
    metadata.serverHeader = serverHeader.trimmed();
    metadata.serverVersion = parseKiwiVersionFromServerHeader(serverHeader);
    if (!metadata.serverHeader.isEmpty()) {
        appendUniqueStatusField(&metadata.stableStatusFields,
                                QStringLiteral("server"),
                                metadata.serverHeader);
    }

    const QList<QByteArray> lines = payload.split('\n');
    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        MsgToken token;
        token.key = QString::fromLatin1(line.left(eq)).trimmed();
        token.value = QString::fromLatin1(line.mid(eq + 1)).trimmed();
        token.hasValue = true;
        updateReceiverMetadataFromMsgToken(token, &metadata);
    }
    if (metadata.hasUsers && metadata.hasUsersMax && metadata.usersMax > 0
        && metadata.users >= metadata.usersMax
        && (!metadata.hasPreempt || metadata.preempt <= 0)) {
        metadata.busy = true;
        metadata.hasBusy = true;
    }
    return metadata;
}

bool mergeReceiverMetadata(ReceiverMetadata* target,
                           const ReceiverMetadata& source)
{
    if (!target) {
        return false;
    }

    bool changed = false;
    changed = mergeString(&target->serverHeader, source.serverHeader) || changed;
    changed = mergeString(&target->serverVersion, source.serverVersion) || changed;
    changed = mergeString(&target->serverBuild, source.serverBuild) || changed;
    if (source.hasUsers) {
        changed = setIntField(&target->users, &target->hasUsers,
                              source.users) || changed;
    }
    if (source.hasUsersMax) {
        changed = setIntField(&target->usersMax, &target->hasUsersMax,
                              source.usersMax) || changed;
    }
    if (source.hasPreempt) {
        changed = setIntField(&target->preempt, &target->hasPreempt,
                              source.preempt) || changed;
    }
    if (source.hasExtApi) {
        changed = setIntField(&target->extApi, &target->hasExtApi,
                              source.extApi) || changed;
    }
    if (source.hasBusy) {
        changed = setBoolField(&target->busy, &target->hasBusy,
                               source.busy) || changed;
    }
    if (source.hasCampStatus) {
        changed = setCampStatusField(&target->campStatus,
                                     &target->hasCampStatus,
                                     source.campStatus) || changed;
    }
    if (source.hasCampReceiverChannel) {
        changed = setIntField(&target->campReceiverChannel,
                              &target->hasCampReceiverChannel,
                              source.campReceiverChannel) || changed;
    }
    if (source.hasCampQueuePosition) {
        changed = setIntField(&target->campQueuePosition,
                              &target->hasCampQueuePosition,
                              source.campQueuePosition) || changed;
    }
    if (source.hasCampQueueWaiters) {
        changed = setIntField(&target->campQueueWaiters,
                              &target->hasCampQueueWaiters,
                              source.campQueueWaiters) || changed;
    }
    if (source.hasCampQueueReloadRecommended) {
        changed = setBoolField(&target->campQueueReloadRecommended,
                               &target->hasCampQueueReloadRecommended,
                               source.campQueueReloadRecommended) || changed;
    }
    if (source.hasMaxCampers) {
        changed = setIntField(&target->maxCampers,
                              &target->hasMaxCampers,
                              source.maxCampers) || changed;
    }
    if (source.hasGpsGood) {
        changed = setBoolField(&target->gpsGood, &target->hasGpsGood,
                               source.gpsGood) || changed;
    }
    changed = mergeString(&target->gpsStatus, source.gpsStatus) || changed;
    if (source.hasAdcClipping) {
        changed = setBoolField(&target->adcClipping, &target->hasAdcClipping,
                               source.adcClipping) || changed;
    }
    if (source.hasReportedFrequency) {
        changed = setDoubleField(&target->reportedFrequencyKhz,
                                 &target->hasReportedFrequency,
                                 source.reportedFrequencyKhz) || changed;
    }
    if (source.hasCoverageCenter) {
        changed = setDoubleField(&target->coverageCenterMhz,
                                 &target->hasCoverageCenter,
                                 source.coverageCenterMhz) || changed;
    }
    if (source.hasCoverageBandwidth) {
        changed = setDoubleField(&target->coverageBandwidthMhz,
                                 &target->hasCoverageBandwidth,
                                 source.coverageBandwidthMhz) || changed;
    }
    if (source.hasReceiverChannel) {
        changed = setIntField(&target->receiverChannel,
                              &target->hasReceiverChannel,
                              source.receiverChannel) || changed;
    }
    if (source.hasWaterfallChannels) {
        changed = setIntField(&target->waterfallChannels,
                              &target->hasWaterfallChannels,
                              source.waterfallChannels) || changed;
    }

    const ApiPolicy policy = apiPolicyFor(
        target->hasExtApi ? target->extApi : -1,
        target->hasUsersMax ? target->usersMax : -1);
    if (target->apiPolicy != policy) {
        target->apiPolicy = policy;
        changed = true;
    }

    for (const QString& field : source.stableStatusFields) {
        if (!target->stableStatusFields.contains(field)) {
            target->stableStatusFields.append(field);
            changed = true;
        }
    }
    trimStableStatusFields(&target->stableStatusFields);
    return changed;
}

bool updateReceiverMetadataFromMsgToken(const MsgToken& token,
                                        ReceiverMetadata* metadata)
{
    if (!metadata || token.key.isEmpty()) {
        return false;
    }

    const QString key = token.key.trimmed();
    const QString valueText = token.value.trimmed();
    if (key.isEmpty()) {
        return false;
    }
    if (!token.hasValue) {
        bool changed = false;
        bool structured = false;
        if (key == QStringLiteral("monitor")) {
            structured = true;
            changed = setBoolField(&metadata->busy, &metadata->hasBusy, true)
                || changed;
            changed = setCampStatusField(&metadata->campStatus,
                                         &metadata->hasCampStatus,
                                         CampStatus::Offered) || changed;
        } else if (key == QStringLiteral("camp_disconnect")) {
            structured = true;
            changed = setCampStatusField(&metadata->campStatus,
                                         &metadata->hasCampStatus,
                                         CampStatus::Disconnected) || changed;
        }
        if (structured) {
            trimStableStatusFields(&metadata->stableStatusFields);
            return changed;
        }
        changed = appendUniqueStatusField(&metadata->stableStatusFields,
                                          key,
                                          QString());
        trimStableStatusFields(&metadata->stableStatusFields);
        return changed;
    }

    bool changed = false;
    bool structured = false;
    auto parseInt = [&valueText](bool* ok) {
        return valueText.toInt(ok);
    };
    auto parseDouble = [&valueText](bool* ok) {
        return valueText.toDouble(ok);
    };

    if (key == QStringLiteral("users")) {
        structured = true;
        bool ok = false;
        const int value = parseInt(&ok);
        if (ok) {
            changed = setIntField(&metadata->users, &metadata->hasUsers, value)
                || changed;
        }
    } else if (key == QStringLiteral("users_max")) {
        structured = true;
        bool ok = false;
        const int value = parseInt(&ok);
        if (ok) {
            changed = setIntField(&metadata->usersMax, &metadata->hasUsersMax,
                                  value) || changed;
        }
    } else if (key == QStringLiteral("preempt")) {
        structured = true;
        bool ok = false;
        const int value = parseInt(&ok);
        if (ok) {
            changed = setIntField(&metadata->preempt, &metadata->hasPreempt,
                                  value) || changed;
        }
    } else if (key == QStringLiteral("ext_api")) {
        structured = true;
        bool ok = false;
        const int value = parseInt(&ok);
        if (ok) {
            changed = setIntField(&metadata->extApi, &metadata->hasExtApi,
                                  value) || changed;
        }
    } else if (key == QStringLiteral("too_busy")) {
        structured = true;
        bool ok = false;
        const int value = parseInt(&ok);
        // Only update busy when the value actually parses. A malformed/empty
        // `too_busy=` must NOT be treated as busy=true, or an injected/garbled
        // MSG could falsely mark a reachable receiver as full. Matches the
        // ok-guarded pattern used by users/users_max/preempt/max_camp above.
        if (ok) {
            changed = setBoolField(&metadata->busy, &metadata->hasBusy,
                                   value != 0) || changed;
        }
    } else if (key == QStringLiteral("max_camp")) {
        structured = true;
        bool ok = false;
        const int value = parseInt(&ok);
        if (ok) {
            changed = setIntField(&metadata->maxCampers,
                                  &metadata->hasMaxCampers,
                                  value) || changed;
        }
    } else if (key == QStringLiteral("camp")) {
        structured = true;
        const QStringList fields = valueText.split(QLatin1Char(','));
        bool okValid = false;
        const int okValue = fields.value(0).trimmed().toInt(&okValid);
        const bool accepted = okValid && okValue != 0;
        changed = setCampStatusField(&metadata->campStatus,
                                     &metadata->hasCampStatus,
                                     accepted ? CampStatus::Accepted
                                              : CampStatus::Rejected)
            || changed;
        bool rxOk = false;
        const int rx = fields.value(1).trimmed().toInt(&rxOk);
        if (rxOk) {
            changed = setIntField(&metadata->campReceiverChannel,
                                  &metadata->hasCampReceiverChannel,
                                  rx) || changed;
        }
    } else if (key == QStringLiteral("qpos")) {
        structured = true;
        const QStringList fields = valueText.split(QLatin1Char(','));
        bool posOk = false;
        bool waitersOk = false;
        const int pos = fields.value(0).trimmed().toInt(&posOk);
        const int waiters = fields.value(1).trimmed().toInt(&waitersOk);
        // Only treat this as a queue update when BOTH coupled, displayed values
        // (position + waiters) parse. A malformed/short `qpos` must not flip the
        // status to Queued or half-update the counters, which would render as a
        // nonsensical "position N of <stale> waiters". Matches the too_busy
        // parse discipline; `reload` is an optional hint applied only if present.
        if (posOk && waitersOk) {
            changed = setCampStatusField(&metadata->campStatus,
                                         &metadata->hasCampStatus,
                                         CampStatus::Queued) || changed;
            changed = setIntField(&metadata->campQueuePosition,
                                  &metadata->hasCampQueuePosition,
                                  pos) || changed;
            changed = setIntField(&metadata->campQueueWaiters,
                                  &metadata->hasCampQueueWaiters,
                                  waiters) || changed;
            bool reloadOk = false;
            const int reload = fields.value(2).trimmed().toInt(&reloadOk);
            if (reloadOk) {
                changed = setBoolField(&metadata->campQueueReloadRecommended,
                                       &metadata->hasCampQueueReloadRecommended,
                                       reload != 0) || changed;
            }
        }
    } else if (key == QStringLiteral("audio_camp")) {
        structured = true;
        const QStringList fields = valueText.split(QLatin1Char(','));
        bool disconnectOk = false;
        const int disconnect =
            fields.value(0).trimmed().toInt(&disconnectOk);
        if (disconnectOk && disconnect != 0) {
            changed = setCampStatusField(&metadata->campStatus,
                                         &metadata->hasCampStatus,
                                         CampStatus::AudioStopped) || changed;
        }
    } else if (key == QStringLiteral("gps_good")) {
        structured = true;
        bool ok = false;
        const double value = parseDouble(&ok);
        if (ok) {
            changed = setBoolField(&metadata->gpsGood,
                                   &metadata->hasGpsGood,
                                   value != 0.0) || changed;
        }
    } else if (key == QStringLiteral("gps")
               || key == QStringLiteral("gps_status")) {
        structured = true;
        changed = mergeString(&metadata->gpsStatus, valueText) || changed;
    } else if (key == QStringLiteral("adc_clipping")
               || key == QStringLiteral("adc_overload")
               || key == QStringLiteral("adc_ov")) {
        structured = true;
        bool ok = false;
        const double value = parseDouble(&ok);
        if (ok) {
            changed = setBoolField(&metadata->adcClipping,
                                   &metadata->hasAdcClipping,
                                   value != 0.0) || changed;
        }
    } else if (key == QStringLiteral("freq")) {
        structured = true;
        bool ok = false;
        const double value = parseDouble(&ok);
        if (ok && std::isfinite(value) && value > 0.0) {
            changed = setDoubleField(&metadata->reportedFrequencyKhz,
                                     &metadata->hasReportedFrequency,
                                     value) || changed;
        }
    } else if (key == QStringLiteral("center_freq")) {
        structured = true;
        bool ok = false;
        const double value = parseDouble(&ok);
        const double mhz = ok ? metadataValueToMhz(value) : 0.0;
        if (mhz > 0.0) {
            changed = setDoubleField(&metadata->coverageCenterMhz,
                                     &metadata->hasCoverageCenter,
                                     mhz) || changed;
        }
    } else if (key == QStringLiteral("bandwidth")) {
        structured = true;
        bool ok = false;
        const double value = parseDouble(&ok);
        const double mhz = ok ? metadataValueToMhz(value) : 0.0;
        if (mhz > 0.0) {
            changed = setDoubleField(&metadata->coverageBandwidthMhz,
                                     &metadata->hasCoverageBandwidth,
                                     mhz) || changed;
        }
    } else if (key == QStringLiteral("rx_chan")) {
        structured = true;
        bool ok = false;
        const int value = parseInt(&ok);
        if (ok) {
            changed = setIntField(&metadata->receiverChannel,
                                  &metadata->hasReceiverChannel,
                                  value) || changed;
        }
    } else if (key == QStringLiteral("wf_chans")
               || key == QStringLiteral("wf_chans_real")) {
        structured = true;
        bool ok = false;
        const int value = parseInt(&ok);
        if (ok) {
            changed = setIntField(&metadata->waterfallChannels,
                                  &metadata->hasWaterfallChannels,
                                  value) || changed;
        }
    } else if (key == QStringLiteral("version")
               || key == QStringLiteral("kiwi_version")
               || key == QStringLiteral("server_version")) {
        structured = true;
        changed = mergeString(&metadata->serverVersion, valueText) || changed;
    } else if (key == QStringLiteral("build")
               || key == QStringLiteral("build_date")
               || key == QStringLiteral("version_build")) {
        structured = true;
        changed = mergeString(&metadata->serverBuild, valueText) || changed;
    }

    if (!structured && shouldRetainUnstructuredStatusField(key, token.hasValue)) {
        changed = appendUniqueStatusField(&metadata->stableStatusFields,
                                          key,
                                          valueText) || changed;
    }

    const ApiPolicy policy = apiPolicyFor(
        metadata->hasExtApi ? metadata->extApi : -1,
        metadata->hasUsersMax ? metadata->usersMax : -1);
    if (metadata->apiPolicy != policy) {
        metadata->apiPolicy = policy;
        changed = true;
    }

    trimStableStatusFields(&metadata->stableStatusFields);
    return changed;
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
