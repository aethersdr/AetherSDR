#include "core/tnc/Ax25AudioCapture.h"

#include "core/SettingsPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <limits>

namespace AetherSDR {
namespace {

bool validCaptureId(const QString& captureId)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9]{8}-[0-9]{6}Z$"));
    return pattern.match(captureId).hasMatch();
}

QString fileNameFor(Ax25AudioCaptureStage stage,
                    const QString& captureId,
                    int packetSequence)
{
    switch (stage) {
    case Ax25AudioCaptureStage::Rx:
        if (packetSequence != 0) {
            return {};
        }
        return QStringLiteral("ax25-rx-capture-%1-float32.wav")
            .arg(captureId);
    case Ax25AudioCaptureStage::TxGenerated:
        if (packetSequence <= 0) {
            return {};
        }
        return QStringLiteral("ax25-tx-generated-%1-%2-float32.wav")
            .arg(captureId)
            .arg(packetSequence, 3, 10, QLatin1Char('0'));
    case Ax25AudioCaptureStage::TxIcomPostResample:
        if (packetSequence <= 0) {
            return {};
        }
        return QStringLiteral("ax25-tx-icom-post-resample-%1-%2-float32.wav")
            .arg(captureId)
            .arg(packetSequence, 3, 10, QLatin1Char('0'));
    }
    return {};
}

void setError(QString* error, const QString& message)
{
    if (error) {
        *error = message;
    }
}

} // namespace

QString makeAx25AudioCaptureId()
{
    return QDateTime::currentDateTimeUtc()
        .toString(QStringLiteral("yyyyMMdd-HHmmss'Z'"));
}

QString ax25AudioCapturePath(Ax25AudioCaptureStage stage,
                             const QString& captureId,
                             int packetSequence)
{
    if (!validCaptureId(captureId)) {
        return {};
    }
    const QString fileName = fileNameFor(stage, captureId, packetSequence);
    if (fileName.isEmpty()) {
        return {};
    }
    return QDir(SettingsPaths::configDir()).filePath(fileName);
}

bool writeAx25Float32Wav(const QString& path,
                         const QByteArray& pcm,
                         int sampleRate,
                         int channels,
                         QString* error)
{
    if (path.isEmpty()) {
        setError(error, QStringLiteral("capture path is empty"));
        return false;
    }
    if (sampleRate <= 0 || sampleRate > 384000) {
        setError(error, QStringLiteral("invalid sample rate %1").arg(sampleRate));
        return false;
    }
    if (channels <= 0 || channels > 8) {
        setError(error, QStringLiteral("invalid channel count %1").arg(channels));
        return false;
    }
    const qsizetype frameBytes =
        static_cast<qsizetype>(channels) * static_cast<qsizetype>(sizeof(float));
    if (pcm.isEmpty() || pcm.size() % frameBytes != 0) {
        setError(error, QStringLiteral("float32 PCM is empty or frame-misaligned"));
        return false;
    }
    if (pcm.size() > static_cast<qsizetype>(std::numeric_limits<quint32>::max() - 36u)) {
        setError(error, QStringLiteral("PCM is too large for RIFF/WAVE"));
        return false;
    }

    const QFileInfo fileInfo(path);
    if (!QDir().mkpath(fileInfo.absolutePath())) {
        setError(error, QStringLiteral("could not create capture directory %1")
                            .arg(fileInfo.absolutePath()));
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }

    auto writeAscii = [&file](const char* text) {
        return file.write(text, 4) == 4;
    };
    auto writeU16 = [&file](quint16 value) {
        const char bytes[2] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff),
        };
        return file.write(bytes, sizeof(bytes)) == sizeof(bytes);
    };
    auto writeU32 = [&file](quint32 value) {
        const char bytes[4] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff),
            static_cast<char>((value >> 16) & 0xff),
            static_cast<char>((value >> 24) & 0xff),
        };
        return file.write(bytes, sizeof(bytes)) == sizeof(bytes);
    };

    constexpr quint16 kAudioFormatIeeeFloat = 3;
    constexpr quint16 kBitsPerSample = 32;
    const quint32 dataBytes = static_cast<quint32>(pcm.size());
    const quint16 blockAlign = static_cast<quint16>(frameBytes);
    const quint32 byteRate = static_cast<quint32>(
        static_cast<quint64>(sampleRate) * static_cast<quint64>(blockAlign));

    const bool headerOk =
        writeAscii("RIFF")
        && writeU32(36u + dataBytes)
        && writeAscii("WAVE")
        && writeAscii("fmt ")
        && writeU32(16u)
        && writeU16(kAudioFormatIeeeFloat)
        && writeU16(static_cast<quint16>(channels))
        && writeU32(static_cast<quint32>(sampleRate))
        && writeU32(byteRate)
        && writeU16(blockAlign)
        && writeU16(kBitsPerSample)
        && writeAscii("data")
        && writeU32(dataBytes);
    if (!headerOk || file.write(pcm) != pcm.size()) {
        setError(error, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

} // namespace AetherSDR
