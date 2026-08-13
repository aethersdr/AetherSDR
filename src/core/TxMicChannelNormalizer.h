#pragma once

#include <QByteArray>

#include <cstdint>

namespace AetherSDR::TxMicChannelNormalizer {

// The largest realtime block this normalizer will accept. Sized to match the
// pull-mode capture chunk so a bounded read can never be rejected, but owned
// HERE because the radio-native DAX route reaches this validator without going
// through TxCaptureBuffer at all — raising the mic read chunk must not silently
// loosen DSP validation on an unrelated route. TxMicChannelNormalizer.cpp
// static_asserts the relationship in both directions.
inline constexpr qsizetype kMaxRealtimeBlockBytes = 256 * 1024;

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
