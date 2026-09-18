#pragma once

#include <QByteArray>

#include <cstdint>

namespace AetherSDR::TxMicChannelNormalizer {

// The largest Int16 capture block this normalizer will accept. Sized to match
// the pull-mode capture chunk so a bounded read can never be rejected, but
// owned HERE rather than borrowed from TxCaptureBuffer: raising the microphone
// read chunk must not silently loosen DSP validation. TxMicChannelNormalizer.cpp
// static_asserts the relationship in both directions.
inline constexpr qsizetype kMaxRealtimeBlockBytes = 256 * 1024;

// The float32 route has its own, much larger ceiling because it is fed by TCI
// rather than by a bounded device read, and its blocks arrive already
// upsampled. A TCI message is capped at 64 KiB (TciServer kMaxWsMessageBytes);
// the worst legal expansion is int16 mono at the lowest supported client rate,
// where every 2-byte sample becomes three 8-byte stereo float frames — a factor
// of 12, or ~768 KiB. Sizing this to the capture chunk instead would have
// dropped every large frame from a conforming 8 or 12 kHz client (#3306).
inline constexpr qsizetype kMaxRealtimeFloatBlockBytes = 1024 * 1024;

enum class ChannelMode : uint8_t {
    Auto = 0,
    Left,
    Right,
    Average,
    Mono,
};

struct AutoState {
    ChannelMode heldMode{ChannelMode::Average};
    int holdFramesRemaining{0};

    void reset() noexcept
    {
        heldMode = ChannelMode::Average;
        holdFramesRemaining = 0;
    }
};

struct Diagnostics {
    float leftPeak{0.0f};
    float rightPeak{0.0f};
    float leftRms{0.0f};
    float rightRms{0.0f};
    ChannelMode selectedMode{ChannelMode::Mono};
    bool oneSidedStereo{false};
    int inputChannels{0};
    int inputSampleRate{0};
    int frames{0};
    qsizetype inputBytes{0};
    // Trailing bytes dropped because the block did not end on a frame boundary.
    // The whole frames ahead of them are still processed.
    qsizetype partialFrameBytes{0};
    // Set only when the entire block was refused as oversized — a real fault
    // worth logging, unlike an ordinary short or empty block.
    bool inputRejected{false};
};

struct LevelBlock {
    float peak{0.0f};
    double sumSq{0.0};
    int frames{0};
};

QByteArray canonicalizeInt16ToMonoStereo(const QByteArray& input,
                                         int inputChannels,
                                         int inputSampleRate,
                                         ChannelMode requestedMode,
                                         AutoState* autoState,
                                         Diagnostics* diagnostics);

// Float32 capture sibling of canonicalizeInt16ToMonoStereo(): same channel
// selection, same Auto hold state, same diagnostics — but it stays in float,
// so the block handed to TxVoiceProcessor::processCapturedFloat32() has never
// been quantized. Reuses kMaxRealtimeBlockBytes rather than the float ceiling
// below: that ceiling exists for TCI, whose blocks arrive already upsampled,
// whereas a capture block is bounded in BYTES by TxCaptureBuffer::kMaxReadBytes
// whatever its sample width.
QByteArray canonicalizeFloat32ToMonoStereo(const QByteArray& input,
                                           int inputChannels,
                                           int inputSampleRate,
                                           ChannelMode requestedMode,
                                           AutoState* autoState,
                                           Diagnostics* diagnostics);

QByteArray collapseFloat32ToInt16MonoBigEndian(const QByteArray& input,
                                               int inputChannels,
                                               int inputSampleRate,
                                               ChannelMode requestedMode,
                                               AutoState* autoState,
                                               Diagnostics* diagnostics);

LevelBlock measureInt16StereoLevelBlock(const QByteArray& pcm);
float rmsFromLevelBlock(const LevelBlock& block);
float dbfs(float linear);
const char* channelModeName(ChannelMode mode);

} // namespace AetherSDR::TxMicChannelNormalizer
