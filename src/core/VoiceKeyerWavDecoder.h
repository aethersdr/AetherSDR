#pragma once

#include <QByteArray>
#include <QString>

namespace AetherSDR {

// WAV file decoding for the client-side voice keyer (#957, RFC #4214).
//
// WAV files are untrusted input: the RIFF
// chunk walker is defensive — every read is bounds-checked against the
// real file size, chunk sizes are never trusted, and hard caps on file
// size, duration, sample rate, and channel count are enforced before
// any large allocation.  Non-finite samples in float files are zeroed
// so NaN/Inf can never reach the TX chain.
//
// Supported: RIFF/WAVE with PCM 8/16/24/32-bit integer or IEEE
// float32/float64 samples (including WAVE_FORMAT_EXTENSIBLE wrappers),
// 1-8 channels, 8-192 kHz.  Multi-channel audio is downmixed to mono by
// averaging — a DVK recording is logically mono.
//
// Contributed by dk4dj with the #957 client-side keyer patch and carried into
// RFC #4214. Kept free of AudioEngine/RadioModel so the parser is unit-testable
// on its own (see tests/voice_keyer_wav_decoder_test.cpp).
class VoiceKeyerWavDecoder {
public:
    // Boundary caps.
    static constexpr qint64 kMaxFileBytes   = 100LL * 1024 * 1024;
    static constexpr int    kMaxDurationSec = 600;   // 10 minutes
    static constexpr int    kMinSampleRate  = 8000;
    static constexpr int    kMaxSampleRate  = 192000;
    static constexpr int    kMaxChannels    = 8;

    // Header-only probe for duration display.  Returns false with a
    // user-readable error when the file is not a usable WAV.
    static bool probeDurationMs(const QString& filePath,
                                int& durationMs,
                                QString& error);

    // Full decode to mono float32 at the file's native rate.
    static bool decodeToMonoFloat(const QString& filePath,
                                  QByteArray& monoFloat32,
                                  int& sampleRate,
                                  QString& error);

    // Resample mono float32 from srcRate to the 24 kHz duplicated-stereo
    // float32 format AudioEngine::sendModemTxAudio() expects, using the
    // project's r8brain Resampler.  Returns an empty array on failure.
    static QByteArray toTxStereo24k(const QByteArray& monoFloat32, int srcRate);
};

} // namespace AetherSDR
