#include "QsoWavPlayback.h"

#include "QsoPcmConverter.h"
#include "QsoWavFormat.h"

#include <QIODevice>

#include <algorithm>
#include <limits>

namespace AetherSDR {

std::optional<QByteArray> prepareQsoWavPlayback(
    QIODevice& source, const QAudioFormat& sinkFormat, QString* error,
    qint64 maxOutputFrames)
{
    const auto fail = [error](const QString& reason) -> std::optional<QByteArray> {
        if (error) {
            *error = reason;
        }
        return std::nullopt;
    };
    if (sinkFormat.sampleRate() < QsoPcmConverter::kMinOutputRate
        || sinkFormat.sampleRate() > QsoPcmConverter::kMaxOutputRate
        || (sinkFormat.channelCount() != 1 && sinkFormat.channelCount() != 2)
        || (sinkFormat.sampleFormat() != QAudioFormat::Int16
            && sinkFormat.sampleFormat() != QAudioFormat::Float)
        || maxOutputFrames <= 0 || maxOutputFrames > kQsoPlaybackMaxFrames) {
        return fail(QStringLiteral("Unsupported QSO playback sink format or buffer budget"));
    }

    const std::optional<QsoWavFormat> file = parseQsoWav(source, error);
    if (!file) {
        return std::nullopt;
    }
    // RIFF caps frameCount below 2^31. Multiplying by the bounded sink rate
    // stays in qint64; round once for the complete file, never once per block.
    const qint64 outputFrames = (file->frameCount * sinkFormat.sampleRate()
                                 + file->sampleRate / 2) / file->sampleRate;
    const int bytesPerFrame = sinkFormat.channelCount()
        * (sinkFormat.sampleFormat() == QAudioFormat::Float ? 4 : 2);
    if (outputFrames <= 0 || outputFrames > maxOutputFrames
        || outputFrames > std::numeric_limits<qsizetype>::max() / bytesPerFrame) {
        return fail(QStringLiteral("QSO recording exceeds the playback buffer limit"));
    }
    const qsizetype outputBytes = static_cast<qsizetype>(outputFrames * bytesPerFrame);
    const QsoPcmConverter::Configuration conversion{
        file->sampleRate, file->channelCount,
        QsoPcmConverter::InputEncoding::Int16LittleEndian,
        sinkFormat.sampleRate(), sinkFormat.channelCount(),
        sinkFormat.sampleFormat() == QAudioFormat::Float
            ? QsoPcmConverter::OutputEncoding::Float32Native
            : QsoPcmConverter::OutputEncoding::Int16Native};
    QsoPcmConverter converter(conversion);
    if (!converter.isValid()) {
        return fail(QStringLiteral("Cannot prepare QSO playback rate conversion"));
    }

    QByteArray result;
    result.reserve(outputBytes);
    qint64 remaining = file->dataBytes;
    constexpr qint64 kBlockFrames = 4096;
    while (remaining > 0) {
        const qint64 bytes = std::min(remaining, kBlockFrames * file->channelCount * 2);
        QByteArray input(static_cast<qsizetype>(bytes), Qt::Uninitialized);
        qint64 received = 0;
        while (received < bytes) {
            const qint64 count = source.read(input.data() + received, bytes - received);
            if (count <= 0 || count > bytes - received) {
                return fail(QStringLiteral("QSO recording data became unreadable during playback preparation"));
            }
            received += count;
        }
        QByteArray block;
        if (!converter.process(input, block) || block.size() > outputBytes - result.size()) {
            return fail(QStringLiteral("QSO playback conversion failed"));
        }
        result.append(block);
        remaining -= bytes;
    }
    QByteArray tail;
    if (!converter.finish(tail) || tail.size() != outputBytes - result.size()) {
        return fail(QStringLiteral("QSO playback conversion produced an incomplete duration"));
    }
    result.append(tail);
    if (error) {
        error->clear();
    }
    return result;
}

} // namespace AetherSDR
