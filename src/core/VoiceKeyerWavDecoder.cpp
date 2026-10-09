#include "VoiceKeyerWavDecoder.h"

#include "core/Resampler.h"

#include <QFile>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace AetherSDR {

namespace {

quint16 readU16(const char* p) { return qFromLittleEndian<quint16>(p); }
quint32 readU32(const char* p) { return qFromLittleEndian<quint32>(p); }

} // namespace

// ─── WAV decoding — untrusted input, validate at the boundary ────────────────

namespace {

struct WavFormat {
    quint16 formatCode{0};    // 1 = PCM, 3 = IEEE float (after EXTENSIBLE unwrap)
    quint16 channels{0};
    quint32 sampleRate{0};
    quint16 bitsPerSample{0};
};

// Walks the RIFF chunk list and extracts fmt + data.  Returns false with a
// user-readable error on anything malformed.  Never trusts chunk sizes:
// every read is bounds-checked against the actual file size.
bool parseWav(const QString& filePath, WavFormat& fmt,
              qint64& dataOffset, qint64& dataBytes, QString& error)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot open file: %1").arg(f.errorString());
        return false;
    }
    const qint64 fileSize = f.size();
    if (fileSize < 44) {
        error = QStringLiteral("File is too small to be a WAV file.");
        return false;
    }
    if (fileSize > VoiceKeyerWavDecoder::kMaxFileBytes) {
        error = QStringLiteral("File is larger than 100 MB.");
        return false;
    }

    char riff[12];
    if (f.read(riff, 12) != 12
        || std::memcmp(riff, "RIFF", 4) != 0
        || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        error = QStringLiteral("Not a RIFF/WAVE file.");
        return false;
    }

    bool haveFmt = false;
    dataOffset = -1;
    dataBytes = 0;

    qint64 pos = 12;
    while (pos + 8 <= fileSize) {
        if (!f.seek(pos)) break;
        char hdr[8];
        if (f.read(hdr, 8) != 8) break;
        const quint32 chunkSize = readU32(hdr + 4);
        const qint64 body = pos + 8;

        if (std::memcmp(hdr, "fmt ", 4) == 0) {
            if (chunkSize < 16 || body + 16 > fileSize) {
                error = QStringLiteral("Malformed fmt chunk.");
                return false;
            }
            char fbuf[40] = {};
            const qint64 want = std::min<qint64>(chunkSize, sizeof(fbuf));
            if (f.read(fbuf, want) != want) {
                error = QStringLiteral("Truncated fmt chunk.");
                return false;
            }
            fmt.formatCode    = readU16(fbuf + 0);
            fmt.channels      = readU16(fbuf + 2);
            fmt.sampleRate    = readU32(fbuf + 4);
            fmt.bitsPerSample = readU16(fbuf + 14);
            // WAVE_FORMAT_EXTENSIBLE: the real format code is the first two
            // bytes of the SubFormat GUID at offset 24.
            if (fmt.formatCode == 0xFFFE) {
                if (chunkSize < 40) {
                    error = QStringLiteral("Malformed WAVE_FORMAT_EXTENSIBLE fmt chunk.");
                    return false;
                }
                fmt.formatCode = readU16(fbuf + 24);
            }
            haveFmt = true;
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            dataOffset = body;
            dataBytes = std::min<qint64>(chunkSize, fileSize - body);
        }

        // Chunks are word-aligned; a zero-size chunk would loop forever.
        qint64 advance = 8 + chunkSize + (chunkSize & 1);
        if (advance <= 8)
            advance = 9;
        pos += advance;
    }

    if (!haveFmt) {
        error = QStringLiteral("No fmt chunk found.");
        return false;
    }
    if (dataOffset < 0 || dataBytes <= 0) {
        error = QStringLiteral("No audio data found.");
        return false;
    }

    if (fmt.formatCode != 1 && fmt.formatCode != 3) {
        error = QStringLiteral("Unsupported WAV encoding (only PCM and IEEE "
                               "float are supported).");
        return false;
    }
    if (fmt.channels < 1 || fmt.channels > VoiceKeyerWavDecoder::kMaxChannels) {
        error = QStringLiteral("Unsupported channel count (%1).").arg(fmt.channels);
        return false;
    }
    if (fmt.sampleRate < static_cast<quint32>(VoiceKeyerWavDecoder::kMinSampleRate)
        || fmt.sampleRate > static_cast<quint32>(VoiceKeyerWavDecoder::kMaxSampleRate)) {
        error = QStringLiteral("Unsupported sample rate (%1 Hz).").arg(fmt.sampleRate);
        return false;
    }
    const bool okBits = (fmt.formatCode == 1
                             && (fmt.bitsPerSample == 8 || fmt.bitsPerSample == 16
                                 || fmt.bitsPerSample == 24 || fmt.bitsPerSample == 32))
                        || (fmt.formatCode == 3
                             && (fmt.bitsPerSample == 32 || fmt.bitsPerSample == 64));
    if (!okBits) {
        error = QStringLiteral("Unsupported sample format (%1-bit, code %2).")
                    .arg(fmt.bitsPerSample).arg(fmt.formatCode);
        return false;
    }

    const qint64 frameBytes = static_cast<qint64>(fmt.channels)
        * (fmt.bitsPerSample / 8);
    const qint64 frames = dataBytes / frameBytes;
    if (frames <= 0) {
        error = QStringLiteral("No audio frames in file.");
        return false;
    }
    if (frames / fmt.sampleRate > VoiceKeyerWavDecoder::kMaxDurationSec) {
        error = QStringLiteral("File is longer than 10 minutes.");
        return false;
    }
    return true;
}

} // namespace

bool VoiceKeyerWavDecoder::probeDurationMs(const QString& filePath,
                                        int& durationMs, QString& error)
{
    WavFormat fmt;
    qint64 dataOffset = 0, dataBytes = 0;
    if (!parseWav(filePath, fmt, dataOffset, dataBytes, error))
        return false;
    const qint64 frameBytes = static_cast<qint64>(fmt.channels)
        * (fmt.bitsPerSample / 8);
    durationMs = static_cast<int>((dataBytes / frameBytes) * 1000
                                  / fmt.sampleRate);
    return true;
}

bool VoiceKeyerWavDecoder::decodeToMonoFloat(const QString& filePath,
                                            QByteArray& monoFloat32,
                                            int& sampleRate, QString& error)
{
    WavFormat fmt;
    qint64 dataOffset = 0, dataBytes = 0;
    if (!parseWav(filePath, fmt, dataOffset, dataBytes, error))
        return false;

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(dataOffset)) {
        error = QStringLiteral("Cannot read audio data.");
        return false;
    }
    const QByteArray raw = f.read(dataBytes);
    if (raw.size() != dataBytes) {
        error = QStringLiteral("Truncated audio data.");
        return false;
    }

    const int ch = fmt.channels;
    const int bytesPerSample = fmt.bitsPerSample / 8;
    const qsizetype frames = raw.size() / (static_cast<qsizetype>(ch) * bytesPerSample);

    monoFloat32.resize(frames * static_cast<qsizetype>(sizeof(float)));
    auto* out = reinterpret_cast<float*>(monoFloat32.data());
    const char* in = raw.constData();

    // Downmix all channels equally to mono (a stereo DVK recording is
    // logically mono; averaging matches the standard downmix).
    const float chScale = 1.0f / static_cast<float>(ch);
    for (qsizetype i = 0; i < frames; ++i) {
        float acc = 0.0f;
        const char* frame = in + i * ch * bytesPerSample;
        for (int c = 0; c < ch; ++c) {
            const char* p = frame + c * bytesPerSample;
            float v = 0.0f;
            if (fmt.formatCode == 1) {
                switch (fmt.bitsPerSample) {
                case 8:
                    v = (static_cast<int>(static_cast<quint8>(*p)) - 128) / 128.0f;
                    break;
                case 16:
                    v = static_cast<qint16>(readU16(p)) / 32768.0f;
                    break;
                case 24: {
                    qint32 s = (static_cast<quint8>(p[0]))
                             | (static_cast<quint8>(p[1]) << 8)
                             | (static_cast<qint8>(p[2]) << 16);
                    v = s / 8388608.0f;
                    break;
                }
                case 32:
                    v = static_cast<qint32>(readU32(p)) / 2147483648.0f;
                    break;
                default:
                    break;
                }
            } else {   // IEEE float
                if (fmt.bitsPerSample == 32) {
                    quint32 bits = readU32(p);
                    float fv;
                    std::memcpy(&fv, &bits, sizeof(fv));
                    v = fv;
                } else {   // 64
                    quint64 bits = qFromLittleEndian<quint64>(p);
                    double dv;
                    std::memcpy(&dv, &bits, sizeof(dv));
                    v = static_cast<float>(dv);
                }
            }
            acc += v;
        }
        float s = acc * chScale;
        if (!std::isfinite(s))
            s = 0.0f;   // NaN/Inf in a float WAV must never reach the TX chain
        out[i] = std::clamp(s, -1.0f, 1.0f);
    }

    sampleRate = static_cast<int>(fmt.sampleRate);
    return true;
}


QByteArray VoiceKeyerWavDecoder::toTxStereo24k(const QByteArray& monoFloat32,
                                               int srcRate)
{
    constexpr int kTxRate = 24000;
    constexpr qsizetype kTxFrameBytes = 2 * static_cast<qsizetype>(sizeof(float));

    const auto* monoF = reinterpret_cast<const float*>(monoFloat32.constData());
    const qsizetype monoSamples =
        monoFloat32.size() / static_cast<qsizetype>(sizeof(float));
    if (monoSamples <= 0 || srcRate < kMinSampleRate || srcRate > kMaxSampleRate)
        return {};

    QByteArray stereo;
    if (srcRate == kTxRate) {
        // Interleave to duplicated stereo without resampling.
        stereo.resize(monoSamples * 2 * static_cast<qsizetype>(sizeof(float)));
        auto* out = reinterpret_cast<float*>(stereo.data());
        for (qsizetype i = 0; i < monoSamples; ++i) {
            out[i * 2]     = monoF[i];
            out[i * 2 + 1] = monoF[i];
        }
    } else {
        // r8brain is a streaming resampler with internal latency: keep
        // feeding blocks (then silence) until the expected output length
        // has drained.
        constexpr int kBlock = 4096;
        Resampler resampler(srcRate, kTxRate, kBlock);
        const qsizetype expectedFrames = static_cast<qsizetype>(
            static_cast<double>(monoSamples) * kTxRate / srcRate);
        const qsizetype expectedBytes =
            expectedFrames * 2 * static_cast<qsizetype>(sizeof(float));
        stereo.reserve(expectedBytes);

        qsizetype fed = 0;
        static const std::array<float, kBlock> kSilence{};
        int flushGuard = 64;   // hard cap on flush iterations
        while (stereo.size() < expectedBytes && flushGuard > 0) {
            if (fed < monoSamples) {
                const int n = static_cast<int>(
                    std::min<qsizetype>(kBlock, monoSamples - fed));
                stereo.append(resampler.processMonoToStereo(monoF + fed, n));
                fed += n;
            } else {
                stereo.append(resampler.processMonoToStereo(kSilence.data(), kBlock));
                --flushGuard;
            }
        }
        if (stereo.size() > expectedBytes)
            stereo.truncate(expectedBytes);
    }

    // Trim to whole TX frames so pacing math stays aligned.
    stereo.truncate(stereo.size() - (stereo.size() % kTxFrameBytes));
    return stereo;
}

} // namespace AetherSDR
