#pragma once

#include <QByteArray>

#include <cstdint>

namespace AetherSDR::TxMicChannelNormalizer {

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
