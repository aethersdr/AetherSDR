// Unit tests for VoiceKeyerWavDecoder (#957) — the untrusted-input
// boundary of the client-side voice keyer.
//
// Covers: PCM 8/16/24/32-bit and IEEE float32/float64 decoding,
// WAVE_FORMAT_EXTENSIBLE unwrapping, stereo→mono downmix, extra RIFF
// chunks before data, duration probing, the 24 kHz duplicated-stereo
// resample path, and rejection of malformed / oversized / overlong /
// unsupported files.  No radio, no AudioEngine — pure file-in, PCM-out.

#include "core/VoiceKeyerWavDecoder.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using AetherSDR::VoiceKeyerWavDecoder;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg);      \
            ++g_failures;                                                   \
        }                                                                   \
    } while (false)

void appendU16(QByteArray& b, quint16 v)
{
    quint16 le = qToLittleEndian(v);
    b.append(reinterpret_cast<const char*>(&le), 2);
}

void appendU32(QByteArray& b, quint32 v)
{
    quint32 le = qToLittleEndian(v);
    b.append(reinterpret_cast<const char*>(&le), 4);
}

// Builds a minimal RIFF/WAVE file.  formatCode 1 = PCM, 3 = float;
// extensible=true wraps the code in WAVE_FORMAT_EXTENSIBLE (0xFFFE).
QByteArray buildWav(quint16 formatCode, quint16 channels, quint32 rate,
                    quint16 bits, const QByteArray& data,
                    bool extensible = false, bool extraChunk = false)
{
    QByteArray fmt;
    appendU16(fmt, extensible ? 0xFFFE : formatCode);
    appendU16(fmt, channels);
    appendU32(fmt, rate);
    appendU32(fmt, rate * channels * bits / 8);   // byte rate
    appendU16(fmt, static_cast<quint16>(channels * bits / 8));
    appendU16(fmt, bits);
    if (extensible) {
        appendU16(fmt, 22);     // cbSize
        appendU16(fmt, bits);   // valid bits
        appendU32(fmt, 0);      // channel mask
        appendU16(fmt, formatCode);   // SubFormat GUID first two bytes
        fmt.append(QByteArray::fromHex("000000001000800000aa00389b71"));
    }

    QByteArray body;
    body.append("WAVE", 4);
    if (extraChunk) {           // e.g. a LIST chunk before fmt/data
        body.append("LIST", 4);
        appendU32(body, 4);
        body.append("INFO", 4);
    }
    body.append("fmt ", 4);
    appendU32(body, static_cast<quint32>(fmt.size()));
    body.append(fmt);
    if (fmt.size() & 1) body.append('\0');
    body.append("data", 4);
    appendU32(body, static_cast<quint32>(data.size()));
    body.append(data);

    QByteArray file;
    file.append("RIFF", 4);
    appendU32(file, static_cast<quint32>(body.size()));
    file.append(body);
    return file;
}

QString writeFile(const QDir& dir, const QString& name, const QByteArray& bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(bytes);
    f.close();
    return path;
}

} // namespace

int main()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "temp dir must be creatable");
    const QDir dir(tmp.path());

    // ── 16-bit PCM stereo: decode + downmix ─────────────────────────────
    {
        // 4 frames: L/R pairs whose averages are 0.25, -0.25, 0.5, 0.0
        QByteArray data;
        const qint16 samples[] = { 16384, 0,   0, -16384,   16384, 16384,   16384, -16384 };
        for (qint16 s : samples) appendU16(data, static_cast<quint16>(s));
        const QString path = writeFile(dir, "pcm16_stereo.wav",
                                       buildWav(1, 2, 48000, 16, data));

        QByteArray mono; int rate = 0; QString error;
        CHECK(VoiceKeyerWavDecoder::decodeToMonoFloat(path, mono, rate, error),
              "pcm16 stereo must decode");
        CHECK(rate == 48000, "rate must be 48 kHz");
        CHECK(mono.size() == 4 * static_cast<int>(sizeof(float)),
              "must yield 4 mono samples");
        const auto* m = reinterpret_cast<const float*>(mono.constData());
        CHECK(std::fabs(m[0] - 0.25f) < 0.001f, "downmix frame 0");
        CHECK(std::fabs(m[1] + 0.25f) < 0.001f, "downmix frame 1");
        CHECK(std::fabs(m[2] - 0.5f)  < 0.001f, "downmix frame 2");
        CHECK(std::fabs(m[3])         < 0.001f, "downmix frame 3");

        int durationMs = -1;
        CHECK(VoiceKeyerWavDecoder::probeDurationMs(path, durationMs, error),
              "probe must succeed");
        CHECK(durationMs == 0, "4 frames at 48 kHz rounds to 0 ms");
    }

    // ── float32 mono via WAVE_FORMAT_EXTENSIBLE + extra chunk ───────────
    {
        QByteArray data;
        const float samples[] = { 0.5f, -0.5f, 1.5f,           // 1.5 must clamp
                                  std::nanf(""), 0.25f };      // NaN must zero
        for (float s : samples) {
            quint32 bits; std::memcpy(&bits, &s, 4);
            appendU32(data, bits);
        }
        const QString path = writeFile(dir, "float32_ext.wav",
            buildWav(3, 1, 44100, 32, data, /*extensible=*/true,
                     /*extraChunk=*/true));

        QByteArray mono; int rate = 0; QString error;
        CHECK(VoiceKeyerWavDecoder::decodeToMonoFloat(path, mono, rate, error),
              "extensible float32 must decode");
        CHECK(rate == 44100, "rate must be 44.1 kHz");
        const auto* m = reinterpret_cast<const float*>(mono.constData());
        CHECK(std::fabs(m[0] - 0.5f) < 0.0001f, "float sample 0");
        CHECK(std::fabs(m[1] + 0.5f) < 0.0001f, "float sample 1");
        CHECK(m[2] <= 1.0f, "over-range float must clamp");
        CHECK(m[3] == 0.0f, "NaN must be zeroed, never reach the TX chain");
    }

    // ── 8-bit and 24-bit PCM ─────────────────────────────────────────────
    {
        QByteArray d8;
        d8.append(static_cast<char>(255));   // ≈ +1.0
        d8.append(static_cast<char>(128));   // 0.0
        d8.append(static_cast<char>(0));     // -1.0
        const QString p8 = writeFile(dir, "pcm8.wav", buildWav(1, 1, 8000, 8, d8));
        QByteArray mono; int rate = 0; QString error;
        CHECK(VoiceKeyerWavDecoder::decodeToMonoFloat(p8, mono, rate, error),
              "pcm8 must decode");
        const auto* m8 = reinterpret_cast<const float*>(mono.constData());
        CHECK(std::fabs(m8[1]) < 0.01f, "pcm8 midpoint is silence");
        CHECK(m8[0] > 0.9f && m8[2] < -0.9f, "pcm8 extremes map to ±1");

        QByteArray d24;
        // +0.5 (0x400000) and -0.5 (0xC00000 sign-extended)
        d24.append('\x00'); d24.append('\x00'); d24.append('\x40');
        d24.append('\x00'); d24.append('\x00'); d24.append('\xC0');
        const QString p24 = writeFile(dir, "pcm24.wav", buildWav(1, 1, 8000, 24, d24));
        CHECK(VoiceKeyerWavDecoder::decodeToMonoFloat(p24, mono, rate, error),
              "pcm24 must decode");
        const auto* m24 = reinterpret_cast<const float*>(mono.constData());
        CHECK(std::fabs(m24[0] - 0.5f) < 0.001f, "pcm24 +0.5");
        CHECK(std::fabs(m24[1] + 0.5f) < 0.001f, "pcm24 -0.5");
    }

    // ── Resample path: 48 kHz → 24 kHz duplicated stereo ────────────────
    {
        // 4800 samples (100 ms) of a 1 kHz sine at 48 kHz.
        QByteArray mono(4800 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
        auto* m = reinterpret_cast<float*>(mono.data());
        for (int i = 0; i < 4800; ++i)
            m[i] = 0.5f * std::sin(2.0 * M_PI * 1000.0 * i / 48000.0);

        const QByteArray stereo = VoiceKeyerWavDecoder::toTxStereo24k(mono, 48000);
        const qsizetype frames = stereo.size() / (2 * sizeof(float));
        CHECK(std::llabs(frames - 2400) <= 4,
              "100 ms must resample to ~2400 frames at 24 kHz");
        const auto* s = reinterpret_cast<const float*>(stereo.constData());
        bool duplicated = true;
        for (qsizetype i = 0; i < frames; ++i)
            duplicated = duplicated && (s[i * 2] == s[i * 2 + 1]);
        CHECK(duplicated, "L and R must carry the identical mono signal");

        // Same-rate path: no resampler involved, exact length.
        const QByteArray same = VoiceKeyerWavDecoder::toTxStereo24k(mono, 24000);
        CHECK(same.size() == mono.size() * 2, "24 kHz input only interleaves");
    }

    // ── Rejection: malformed / unsupported / overlong ────────────────────
    {
        QByteArray mono; int rate = 0; int durationMs = 0; QString error;

        CHECK(!VoiceKeyerWavDecoder::probeDurationMs(
                  dir.filePath("does_not_exist.wav"), durationMs, error),
              "missing file must be rejected");

        const QString garbage = writeFile(dir, "garbage.wav",
            QByteArray(4096, '\x5A'));
        CHECK(!VoiceKeyerWavDecoder::decodeToMonoFloat(garbage, mono, rate, error),
              "non-RIFF garbage must be rejected");

        QByteArray tiny = buildWav(1, 1, 8000, 16, QByteArray(64, '\0'));
        tiny.truncate(20);
        const QString truncated = writeFile(dir, "truncated.wav", tiny);
        CHECK(!VoiceKeyerWavDecoder::decodeToMonoFloat(truncated, mono, rate, error),
              "truncated header must be rejected");

        const QString ulaw = writeFile(dir, "ulaw.wav",
            buildWav(7 /* µ-law */, 1, 8000, 8, QByteArray(64, '\0')));
        CHECK(!VoiceKeyerWavDecoder::decodeToMonoFloat(ulaw, mono, rate, error),
              "compressed encodings must be rejected");

        const QString badRate = writeFile(dir, "bad_rate.wav",
            buildWav(1, 1, 4000, 16, QByteArray(64, '\0')));
        CHECK(!VoiceKeyerWavDecoder::decodeToMonoFloat(badRate, mono, rate, error),
              "sub-8 kHz sample rate must be rejected");

        const QString tooManyCh = writeFile(dir, "many_ch.wav",
            buildWav(1, 12, 48000, 16, QByteArray(48, '\0')));
        CHECK(!VoiceKeyerWavDecoder::decodeToMonoFloat(tooManyCh, mono, rate, error),
              "more than 8 channels must be rejected");

        // > 10 minutes: 8 kHz mono 8-bit, 601 s of data ≈ 4.7 MB.
        const QString overlong = writeFile(dir, "overlong.wav",
            buildWav(1, 1, 8000, 8, QByteArray(8000 * 601, '\x80')));
        CHECK(!VoiceKeyerWavDecoder::probeDurationMs(overlong, durationMs, error),
              "files longer than 10 minutes must be rejected");

        // data chunk claiming more bytes than the file holds: must not
        // crash and must decode only what is really there (bounds check).
        QByteArray lying = buildWav(1, 1, 8000, 16, QByteArray(32, '\0'));
        const int dataSizeOffset = lying.indexOf("data") + 4;
        quint32 huge = qToLittleEndian<quint32>(0x7FFFFFFF);
        std::memcpy(lying.data() + dataSizeOffset, &huge, 4);
        const QString liar = writeFile(dir, "lying_size.wav", lying);
        if (VoiceKeyerWavDecoder::decodeToMonoFloat(liar, mono, rate, error)) {
            CHECK(mono.size() == 16 * static_cast<int>(sizeof(float)),
                  "oversized data-chunk claim must clamp to real bytes");
        }
    }

    if (g_failures == 0) {
        std::printf("voice_keyer_wav_decoder_test: all checks passed\n");
        return 0;
    }
    std::printf("voice_keyer_wav_decoder_test: %d check(s) FAILED\n", g_failures);
    return 1;
}
