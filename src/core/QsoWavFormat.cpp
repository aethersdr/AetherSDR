#include "QsoWavFormat.h"

#include <QIODevice>
#include <QtEndian>

#include <array>
#include <cstring>

namespace {

constexpr int kMaxChunks = 4096;

bool readAt(QIODevice& source, qint64 offset, char* output, qint64 length)
{
    if (!source.seek(offset)) {
        return false;
    }
    qint64 received = 0;
    while (received < length) {
        const qint64 count = source.read(output + received, length - received);
        if (count <= 0 || count > length - received) {
            return false;
        }
        received += count;
    }
    return true;
}

bool isFourCc(const char* bytes, const char* expected)
{
    return std::memcmp(bytes, expected, 4) == 0;
}

} // namespace

namespace AetherSDR {

std::optional<QsoWavFormat> parseQsoWav(QIODevice& source, QString* error)
{
    const auto fail = [error](const QString& reason) -> std::optional<QsoWavFormat> {
        if (error) {
            *error = reason;
        }
        return std::nullopt;
    };

    if (!source.isOpen() || !source.isReadable() || source.isSequential()
        || source.isTextModeEnabled()) {
        return fail(QStringLiteral("WAV source must be open, readable, binary and seekable"));
    }

    const qint64 fileBytes = source.size();
    std::array<char, 12> riffHeader{};
    if (fileBytes < 12 || !readAt(source, 0, riffHeader.data(), riffHeader.size())) {
        return fail(QStringLiteral("Missing or unreadable RIFF header"));
    }
    if (!isFourCc(riffHeader.data(), "RIFF") || !isFourCc(riffHeader.data() + 8, "WAVE")) {
        return fail(QStringLiteral("Unsupported WAV container; expected RIFF/WAVE"));
    }

    // RIFF chunk sizes omit their eight-byte header; subchunk sizes also omit
    // the padding byte. Widen BEFORE adding, including the classic 4 GiB edge.
    // https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-
    const qint64 riffBytes = qFromLittleEndian<quint32>(riffHeader.data() + 4);
    const qint64 riffEnd = 8 + riffBytes;
    if (riffBytes < 4 || riffEnd > fileBytes) {
        return fail(QStringLiteral("RIFF size exceeds the source or omits its WAVE type"));
    }

    QsoWavFormat format;
    bool foundFormat = false;
    bool foundData = false;
    int chunksScanned = 0;
    qint64 chunkOffset = 12;
    while (chunkOffset < riffEnd) {
        if (chunksScanned == kMaxChunks) {
            return fail(QStringLiteral("WAV exceeds the 4096-chunk scan limit"));
        }
        ++chunksScanned;
        if (riffEnd - chunkOffset < 8) {
            return fail(QStringLiteral("Truncated WAV chunk header"));
        }
        std::array<char, 8> chunkHeader{};
        if (!readAt(source, chunkOffset, chunkHeader.data(), chunkHeader.size())) {
            return fail(QStringLiteral("Cannot read WAV chunk header"));
        }
        const qint64 chunkBytes = qFromLittleEndian<quint32>(chunkHeader.data() + 4);
        const qint64 payloadOffset = chunkOffset + 8;
        const qint64 paddedBytes = chunkBytes + (chunkBytes & 1);
        if (paddedBytes > riffEnd - payloadOffset) {
            return fail(QStringLiteral("WAV chunk or padding exceeds the RIFF boundary"));
        }

        if (isFourCc(chunkHeader.data(), "fmt ")) {
            if (foundFormat) {
                return fail(QStringLiteral("Duplicate WAV format chunk"));
            }
            if (chunkBytes < 16) {
                return fail(QStringLiteral("WAV format chunk is shorter than PCM format fields"));
            }
            std::array<char, 16> pcmHeader{};
            if (!readAt(source, payloadOffset, pcmHeader.data(), pcmHeader.size())) {
                return fail(QStringLiteral("Cannot read WAV PCM format fields"));
            }
            const quint16 encoding = qFromLittleEndian<quint16>(pcmHeader.data());
            const quint16 channels = qFromLittleEndian<quint16>(pcmHeader.data() + 2);
            const quint32 rate = qFromLittleEndian<quint32>(pcmHeader.data() + 4);
            const quint32 byteRate = qFromLittleEndian<quint32>(pcmHeader.data() + 8);
            const quint16 blockAlign = qFromLittleEndian<quint16>(pcmHeader.data() + 12);
            const quint16 bits = qFromLittleEndian<quint16>(pcmHeader.data() + 14);
            if (encoding != 1 || bits != 16) {
                return fail(QStringLiteral("Only uncompressed PCM16 WAV is supported"));
            }
            if (channels != 1 && channels != 2) {
                return fail(QStringLiteral("Only mono or stereo WAV is supported"));
            }
            if (rate != 24000 && rate != 44100 && rate != 48000) {
                return fail(QStringLiteral("Unsupported WAV sample rate"));
            }
            if (blockAlign != channels * 2 || byteRate != rate * blockAlign) {
                return fail(QStringLiteral("Inconsistent WAV block alignment or byte rate"));
            }
            // WAVE_FORMAT_PCM ignores WAVEFORMATEX::cbSize. The first 16
            // bytes carry its complete format; skip any bounded extension.
            // https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/ns-mmeapi-waveformatex
            format.sampleRate = static_cast<int>(rate);
            format.channelCount = static_cast<int>(channels);
            foundFormat = true;
        } else if (isFourCc(chunkHeader.data(), "data")) {
            if (foundData) {
                return fail(QStringLiteral("Duplicate WAV data chunk"));
            }
            if (chunkBytes == 0) {
                return fail(QStringLiteral("WAV data chunk is empty"));
            }
            format.dataOffset = payloadOffset;
            format.dataBytes = chunkBytes;
            foundData = true;
        }
        // Seek over PCM and opaque metadata on the next read. Scanning the
        // rest of RIFF catches duplicate/invalid chunks after the PCM payload.
        chunkOffset = payloadOffset + paddedBytes;
    }

    if (!foundFormat || !foundData) {
        return fail(QStringLiteral("WAV requires one format chunk and one data chunk"));
    }
    const int frameBytes = format.channelCount * 2;
    if (format.dataBytes % frameBytes != 0) {
        return fail(QStringLiteral("WAV data ends within a sample frame"));
    }
    format.frameCount = format.dataBytes / frameBytes;
    if (!source.seek(format.dataOffset)) {
        return fail(QStringLiteral("Cannot seek to WAV PCM data"));
    }
    if (error) {
        error->clear();
    }
    return format;
}

} // namespace AetherSDR
