#pragma once

#include <QByteArray>

#ifdef HAVE_OPUS
struct OpusDecoder;
struct OpusEncoder;
#endif

namespace MasterSDR {

// Thin wrapper around libopus for SmartLink compressed audio.
// Encodes/decodes stereo 24kHz audio in 10ms frames (240 samples).
// Matches SmartSDR's Opus configuration (stereo, ~70kbps, CELT-only SWB).
// Opus payload is raw bytes — no byte-swapping needed in VITA-49.
//
// RX: Opus frame bytes → decode → stereo int16 PCM
// TX: stereo int16 PCM → encode → Opus frame bytes
//
// Requires libopus (bundled via RADE build). No-op stub without HAVE_OPUS.

class OpusCodec {
public:
    OpusCodec();
    ~OpusCodec();

    OpusCodec(const OpusCodec&) = delete;
    OpusCodec& operator=(const OpusCodec&) = delete;

    bool isValid() const;

    // Decode an Opus frame → stereo int16 24kHz PCM (for AudioEngine)
    QByteArray decode(const QByteArray& opusFrame);

    // Synthesize one frame of concealment audio from the decoder's internal
    // state (Opus native PLC, opus_decode(nullptr, 0, ...)). Used when a
    // packet is detected lost via VITA-49 sequence skip; perceptually much
    // better than raw silence for one or two consecutive missed frames. (#2731)
    QByteArray concealLost();

    // Encode stereo int16 24kHz PCM → Opus frame bytes
    QByteArray encode(const QByteArray& pcmStereo);

    // Bitrate in bits/sec (default 32000). Higher = better quality, more bandwidth.
    void setBitrate(int bps);
    int bitrate() const { return m_bitrate; }

private:
    int m_bitrate{70000};

#ifdef HAVE_OPUS
    OpusDecoder* m_decoder{nullptr};
    OpusEncoder* m_encoder{nullptr};
    static constexpr int SAMPLE_RATE = 24000;
    static constexpr int CHANNELS    = 2;      // stereo (matches SmartSDR)
    static constexpr int FRAME_MS    = 10;
    static constexpr int FRAME_SIZE  = SAMPLE_RATE * FRAME_MS / 1000;  // 240 samples
    static constexpr int MAX_OPUS_BYTES = 4000; // max encoded frame size
#endif
};

} // namespace MasterSDR
