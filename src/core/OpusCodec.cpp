#include "OpusCodec.h"
#include "LogManager.h"

#ifdef HAVE_OPUS
#include <opus.h>
#endif

#include <cstring>

namespace AetherSDR {

OpusCodec::OpusCodec()
{
#ifdef HAVE_OPUS
    int err;
    m_decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    if (err != OPUS_OK || !m_decoder)
        qCWarning(lcAudio) << "OpusCodec: decoder create failed:" << opus_strerror(err);

    m_encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !m_encoder) {
        qCWarning(lcAudio) << "OpusCodec: encoder create failed:" << opus_strerror(err);
    } else {
        opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(m_bitrate));
        opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(10));
        opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }
#endif
}

OpusCodec::~OpusCodec()
{
#ifdef HAVE_OPUS
    if (m_decoder) opus_decoder_destroy(m_decoder);
    if (m_encoder) opus_encoder_destroy(m_encoder);
#endif
}

bool OpusCodec::isValid() const
{
#ifdef HAVE_OPUS
    return m_decoder != nullptr && m_encoder != nullptr;
#else
    return false;
#endif
}

QByteArray OpusCodec::decode(const QByteArray& opusFrame)
{
#ifdef HAVE_OPUS
    if (!m_decoder || opusFrame.isEmpty()) return {};

    // Decode Opus → stereo int16 PCM (aligned for SSE)
    alignas(16) int16_t stereoOut[FRAME_SIZE * CHANNELS];
    int samples = opus_decode(m_decoder,
        reinterpret_cast<const unsigned char*>(opusFrame.constData()),
        opusFrame.size(), stereoOut, FRAME_SIZE, 0);

    if (samples <= 0) {
        qCWarning(lcAudio) << "OpusCodec: decode failed:" << opus_strerror(samples);
        return {};
    }

    return QByteArray(reinterpret_cast<const char*>(stereoOut),
                      samples * CHANNELS * sizeof(int16_t));
#else
    Q_UNUSED(opusFrame);
    return {};
#endif
}

QByteArray OpusCodec::concealLost()
{
#ifdef HAVE_OPUS
    if (!m_decoder) return {};

    // opus_decode with a null pointer requests packet-loss concealment:
    // libopus synthesizes one frame from its internal state, producing a
    // perceptually smooth fill rather than the splice click we'd get from
    // jumping straight to the next received frame. (#2731)
    alignas(16) int16_t stereoOut[FRAME_SIZE * CHANNELS];
    int samples = opus_decode(m_decoder, nullptr, 0, stereoOut, FRAME_SIZE, 0);
    if (samples <= 0) return {};

    return QByteArray(reinterpret_cast<const char*>(stereoOut),
                      samples * CHANNELS * sizeof(int16_t));
#else
    return {};
#endif
}

QByteArray OpusCodec::encode(const QByteArray& pcmStereo)
{
#ifdef HAVE_OPUS
    if (!m_encoder || pcmStereo.isEmpty()) return {};

    // Input: stereo int16 interleaved, FRAME_SIZE sample frames (240 × 2 channels)
    int totalSamples = pcmStereo.size() / sizeof(int16_t);
    int frameSamples = totalSamples / CHANNELS;  // per-channel sample count

    // Opus needs exactly FRAME_SIZE samples per channel per encode call
    if (frameSamples != FRAME_SIZE) return {};

    // Copy to 16-byte aligned buffer — opus SSE intrinsics (stereo_merge,
    // _mm_mul_ps) require aligned input. QByteArray::constData() is NOT
    // guaranteed to be 16-byte aligned, causing SEGV on SSE paths.
    alignas(16) int16_t aligned[FRAME_SIZE * CHANNELS];
    std::memcpy(aligned, pcmStereo.constData(), FRAME_SIZE * CHANNELS * sizeof(int16_t));

    // Encode stereo → Opus
    unsigned char opusOut[MAX_OPUS_BYTES];
    int bytes = opus_encode(m_encoder, aligned, FRAME_SIZE,
                            opusOut, MAX_OPUS_BYTES);

    if (bytes <= 0) {
        qCWarning(lcAudio) << "OpusCodec: encode failed:" << opus_strerror(bytes);
        return {};
    }

    return QByteArray(reinterpret_cast<const char*>(opusOut), bytes);
#else
    Q_UNUSED(pcmStereo);
    return {};
#endif
}

void OpusCodec::setBitrate(int bps)
{
    m_bitrate = bps;
#ifdef HAVE_OPUS
    if (m_encoder)
        opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bps));
#endif
}

} // namespace AetherSDR
