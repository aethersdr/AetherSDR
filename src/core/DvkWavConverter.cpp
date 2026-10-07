#include "DvkWavConverter.h"

#include "Resampler.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace AetherSDR::DvkWavConverter {

namespace {

constexpr quint16 kFormatPcm = 1;
constexpr quint16 kFormatFloat = 3;
constexpr quint16 kFormatExtensible = 0xFFFE;

struct WavFormat {
    quint16 tag{0};
    quint16 channels{0};
    quint32 sampleRate{0};
    quint16 blockAlign{0};
    quint16 bitsPerSample{0};
};

template <typename T>
T readLe(const char* data)
{
    return qFromLittleEndian<T>(data);
}

float decodeSample(const char* p, const WavFormat& fmt)
{
    if (fmt.tag == kFormatFloat) {
        float value = 0.0F;
        const quint32 bits = readLe<quint32>(p);
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value) ? value : 0.0F;
    }
    switch (fmt.bitsPerSample) {
    case 8:
        return (static_cast<float>(static_cast<quint8>(*p)) - 128.0F) / 128.0F;
    case 16:
        return static_cast<float>(readLe<qint16>(p)) / 32768.0F;
    case 24: {
        const qint32 raw = static_cast<qint32>(static_cast<quint8>(p[0]))
                         | (static_cast<qint32>(static_cast<quint8>(p[1])) << 8)
                         | (static_cast<qint32>(static_cast<qint8>(p[2])) << 16);
        return static_cast<float>(raw) / 8388608.0F;
    }
    case 32:
        return static_cast<float>(static_cast<double>(readLe<qint32>(p)) / 2147483648.0);
    default:
        return 0.0F;
    }
}

QByteArray encodePcm16Mono(const std::vector<float>& samples)
{
    const quint32 dataBytes = static_cast<quint32>(samples.size() * 2);
    QByteArray out;
    out.reserve(static_cast<qsizetype>(44 + dataBytes));

    const auto put16 = [&out](quint16 v) {
        char b[2];
        qToLittleEndian(v, b);
        out.append(b, 2);
    };
    const auto put32 = [&out](quint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        out.append(b, 4);
    };

    out.append("RIFF", 4);
    put32(36 + dataBytes);
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    put32(16);
    put16(kFormatPcm);
    put16(1);
    put32(kRadioSampleRate);
    put32(kRadioSampleRate * 2);
    put16(2);
    put16(16);
    out.append("data", 4);
    put32(dataBytes);

    for (const float s : samples) {
        // A mix of near-FLT_MAX channels can reach inf, and the resampler NaN;
        // std::clamp passes NaN through, and lround(NaN) is unspecified.
        const float clamped = std::isfinite(s) ? std::clamp(s, -1.0F, 1.0F) : 0.0F;
        put16(static_cast<quint16>(static_cast<qint16>(std::lround(clamped * 32767.0F))));
    }
    return out;
}

}  // namespace

QByteArray convertForRadio(const QByteArray& wav, QString& error)
{
    if (wav.size() < 12 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE") {
        error = QStringLiteral("Not a WAV file");
        return {};
    }

    WavFormat fmt;
    bool haveFmt = false;
    const char* data = nullptr;
    qint64 dataBytes = 0;

    // Chunks may come in any order and include LIST/fact/etc.; each is padded
    // to an even length.
    qint64 pos = 12;
    while (pos + 8 <= wav.size()) {
        const QByteArray id = wav.mid(pos, 4);
        const qint64 size = readLe<quint32>(wav.constData() + pos + 4);
        const qint64 body = pos + 8;
        const qint64 available = std::min<qint64>(size, wav.size() - body);
        if (id == "fmt " && available >= 16) {
            const char* f = wav.constData() + body;
            fmt.tag = readLe<quint16>(f);
            fmt.channels = readLe<quint16>(f + 2);
            fmt.sampleRate = readLe<quint32>(f + 4);
            fmt.blockAlign = readLe<quint16>(f + 12);
            fmt.bitsPerSample = readLe<quint16>(f + 14);
            if (fmt.tag == kFormatExtensible && available >= 26) {
                // The sub-format GUID starts with the real format tag.
                fmt.tag = readLe<quint16>(f + 24);
            }
            haveFmt = true;
        } else if (id == "data" && !data) {
            data = wav.constData() + body;
            dataBytes = std::max<qint64>(available, 0);  // tolerate a truncated size
        }
        pos = body + size + (size & 1);
    }

    if (!haveFmt || !data) {
        error = QStringLiteral("WAV file has no audio format or no audio data");
        return {};
    }

    const bool pcm = fmt.tag == kFormatPcm
        && (fmt.bitsPerSample == 8 || fmt.bitsPerSample == 16
            || fmt.bitsPerSample == 24 || fmt.bitsPerSample == 32);
    const bool floating = fmt.tag == kFormatFloat && fmt.bitsPerSample == 32;
    if (!pcm && !floating) {
        error = QStringLiteral("Unsupported WAV encoding (format %1, %2-bit); use PCM or 32-bit float")
                    .arg(fmt.tag).arg(fmt.bitsPerSample);
        return {};
    }
    const int bytesPerSample = fmt.bitsPerSample / 8;
    if (fmt.channels < 1 || fmt.channels > 8 || fmt.sampleRate < 8'000
        || fmt.sampleRate > 384'000 || fmt.blockAlign < fmt.channels * bytesPerSample) {
        error = QStringLiteral("WAV header is invalid (%1 channels, %2 Hz)")
                    .arg(fmt.channels).arg(fmt.sampleRate);
        return {};
    }

    const qint64 frames = dataBytes / fmt.blockAlign;
    if (frames <= 0) {
        error = QStringLiteral("WAV file contains no audio");
        return {};
    }
    const qint64 durationMs = frames * 1000 / fmt.sampleRate;
    if (durationMs > kMaxDurationMs) {
        error = QStringLiteral("Recording is %1 s long; a DVK slot holds at most %2 s")
                    .arg(static_cast<double>(durationMs) / 1000.0, 0, 'f', 1)
                    .arg(kMaxDurationMs / 1000);
        return {};
    }

    std::vector<float> mono(static_cast<size_t>(frames));
    for (qint64 i = 0; i < frames; ++i) {
        const char* frame = data + i * fmt.blockAlign;
        float sum = 0.0F;
        for (int c = 0; c < fmt.channels; ++c) {
            sum += decodeSample(frame + c * bytesPerSample, fmt);
        }
        mono[static_cast<size_t>(i)] = sum / static_cast<float>(fmt.channels);
    }

    if (fmt.sampleRate == static_cast<quint32>(kRadioSampleRate)) {
        return encodePcm16Mono(mono);
    }

    // Resampler output lags by its group delay; drain() returns the tail, and
    // the leading delay is trimmed so the clip keeps its timing and length.
    const double ratio = static_cast<double>(kRadioSampleRate) / fmt.sampleRate;
    Resampler resampler(fmt.sampleRate, kRadioSampleRate);
    QByteArray converted = resampler.process(mono.data(), static_cast<int>(mono.size()));
    converted.append(resampler.drain());

    const auto* out = reinterpret_cast<const float*>(converted.constData());
    const qint64 outCount = converted.size() / static_cast<qint64>(sizeof(float));
    const qint64 lead = std::llround(resampler.groupDelayInputFrames() * ratio);
    const qint64 wanted = std::llround(static_cast<double>(frames) * ratio);
    const qint64 first = std::min(lead, outCount);
    const qint64 count = std::min(wanted, outCount - first);

    std::vector<float> resampled(out + first, out + first + count);
    // The filter tail can come up a sample short; pad so the length is exact.
    resampled.resize(static_cast<size_t>(wanted), 0.0F);
    return encodePcm16Mono(resampled);
}

}  // namespace AetherSDR::DvkWavConverter
