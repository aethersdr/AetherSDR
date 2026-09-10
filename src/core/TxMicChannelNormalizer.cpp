#include "TxMicChannelNormalizer.h"

#include "TxCaptureBuffer.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR::TxMicChannelNormalizer {

// A bounded capture read must never be refused as oversized, and neither cap
// may grow past the point where the int frame counts below can overflow.
// Asserting here keeps the constants honest if any of them moves.
static_assert(kMaxRealtimeBlockBytes >= TxCaptureBuffer::kMaxReadBytes,
              "a bounded capture read must never be rejected as oversized");
static_assert(kMaxRealtimeBlockBytes / static_cast<qsizetype>(sizeof(int16_t))
                  <= static_cast<qsizetype>(std::numeric_limits<int>::max()),
              "validated Int16 frame counts must stay representable as int");
static_assert(kMaxRealtimeFloatBlockBytes / static_cast<qsizetype>(sizeof(float))
                  <= static_cast<qsizetype>(std::numeric_limits<int>::max()),
              "validated float32 frame counts must stay representable as int");

namespace {
constexpr float kSilenceFloor = 0.0005623413f; // -65 dBFS
constexpr float kWeakToStrongRatio = 0.25118864f; // -12 dB
constexpr int kDefaultSampleRate = 24000;

// Bounds a realtime block before any sample is dereferenced or any output is
// allocated (Principle VII). Oversize is a hard reject: it is the signature of
// an unbounded backend residue, and processing it once overflowed the output
// size calculation. A trailing partial frame is not — it is truncated to the
// frame boundary, as the pre-cap code did, so a momentarily misaligned backend
// costs one sample rather than a whole-block dropout.
bool boundRealtimeBlock(const QByteArray& input,
                        qsizetype bytesPerSample,
                        int channels,
                        qsizetype maxBlockBytes,
                        Diagnostics& diagnostics)
{
    diagnostics.inputBytes = input.size();

    const qsizetype frameBytes = bytesPerSample * channels;
    if (frameBytes <= 0) {
        return false;
    }

    if (input.size() > maxBlockBytes) {
        diagnostics.inputRejected = true;
        return false;
    }

    diagnostics.partialFrameBytes = input.size() % frameBytes;
    return input.size() >= frameBytes;
}

int frameHoldCount(int sampleRate, int frames)
{
    const int rate = sampleRate > 0 ? sampleRate : kDefaultSampleRate;
    return std::max(frames, rate / 4);
}

float finiteOrZero(float value)
{
    return std::isfinite(value) ? value : 0.0f;
}

float absInt16(int16_t value)
{
    return static_cast<float>(std::abs(static_cast<int>(value))) / 32768.0f;
}

int16_t clampInt16(int value)
{
    return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

int16_t averageInt16(int16_t left, int16_t right)
{
    return clampInt16((static_cast<int>(left) + static_cast<int>(right)) / 2);
}

int16_t floatToInt16(float sample)
{
    const float clamped = std::clamp(finiteOrZero(sample), -1.0f, 1.0f);
    return static_cast<int16_t>(std::clamp(static_cast<int>(clamped * 32767.0f),
                                          -32768,
                                          32767));
}

ChannelMode resolveManualMode(ChannelMode requested)
{
    switch (requested) {
    case ChannelMode::Left:
    case ChannelMode::Right:
    case ChannelMode::Average:
        return requested;
    case ChannelMode::Auto:
    case ChannelMode::Mono:
        return ChannelMode::Average;
    }
    return ChannelMode::Average;
}

ChannelMode chooseStereoMode(float leftRms,
                             float rightRms,
                             int inputSampleRate,
                             int frames,
                             ChannelMode requestedMode,
                             AutoState* autoState,
                             bool* oneSided)
{
    if (oneSided) {
        *oneSided = false;
    }

    if (requestedMode != ChannelMode::Auto) {
        if (autoState) {
            autoState->reset();
        }
        return resolveManualMode(requestedMode);
    }

    const bool leftActive = leftRms >= kSilenceFloor;
    const bool rightActive = rightRms >= kSilenceFloor;

    if (leftActive && rightRms <= leftRms * kWeakToStrongRatio) {
        if (autoState) {
            autoState->heldMode = ChannelMode::Left;
            autoState->holdFramesRemaining = frameHoldCount(inputSampleRate, frames);
        }
        if (oneSided) {
            *oneSided = true;
        }
        return ChannelMode::Left;
    }

    if (rightActive && leftRms <= rightRms * kWeakToStrongRatio) {
        if (autoState) {
            autoState->heldMode = ChannelMode::Right;
            autoState->holdFramesRemaining = frameHoldCount(inputSampleRate, frames);
        }
        if (oneSided) {
            *oneSided = true;
        }
        return ChannelMode::Right;
    }

    if (!leftActive && !rightActive && autoState && autoState->holdFramesRemaining > 0) {
        const ChannelMode held = autoState->heldMode;
        autoState->holdFramesRemaining = std::max(0, autoState->holdFramesRemaining - frames);
        if (held == ChannelMode::Left || held == ChannelMode::Right) {
            return held;
        }
    }

    if (autoState) {
        autoState->reset();
    }
    return ChannelMode::Average;
}

void analyzeInt16Mono(const int16_t* src, int frames, Diagnostics& diag)
{
    double sumSq = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float s = absInt16(src[i]);
        peak = std::max(peak, s);
        sumSq += static_cast<double>(s) * s;
    }
    const float rms = frames > 0 ? static_cast<float>(std::sqrt(sumSq / frames)) : 0.0f;
    diag.leftPeak = peak;
    diag.rightPeak = peak;
    diag.leftRms = rms;
    diag.rightRms = rms;
}

void analyzeInt16Stereo(const int16_t* src, int frames, Diagnostics& diag)
{
    double leftSumSq = 0.0;
    double rightSumSq = 0.0;
    float leftPeak = 0.0f;
    float rightPeak = 0.0f;

    for (int i = 0; i < frames; ++i) {
        const float left = absInt16(src[i * 2]);
        const float right = absInt16(src[i * 2 + 1]);
        leftPeak = std::max(leftPeak, left);
        rightPeak = std::max(rightPeak, right);
        leftSumSq += static_cast<double>(left) * left;
        rightSumSq += static_cast<double>(right) * right;
    }

    diag.leftPeak = leftPeak;
    diag.rightPeak = rightPeak;
    diag.leftRms = frames > 0 ? static_cast<float>(std::sqrt(leftSumSq / frames)) : 0.0f;
    diag.rightRms = frames > 0 ? static_cast<float>(std::sqrt(rightSumSq / frames)) : 0.0f;
}

void analyzeFloat32Mono(const float* src, int frames, Diagnostics& diag)
{
    double sumSq = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float s = std::abs(finiteOrZero(src[i]));
        peak = std::max(peak, s);
        sumSq += static_cast<double>(s) * s;
    }
    const float rms = frames > 0 ? static_cast<float>(std::sqrt(sumSq / frames)) : 0.0f;
    diag.leftPeak = peak;
    diag.rightPeak = peak;
    diag.leftRms = rms;
    diag.rightRms = rms;
}

void analyzeFloat32Stereo(const float* src, int frames, Diagnostics& diag)
{
    double leftSumSq = 0.0;
    double rightSumSq = 0.0;
    float leftPeak = 0.0f;
    float rightPeak = 0.0f;

    for (int i = 0; i < frames; ++i) {
        const float left = std::abs(finiteOrZero(src[i * 2]));
        const float right = std::abs(finiteOrZero(src[i * 2 + 1]));
        leftPeak = std::max(leftPeak, left);
        rightPeak = std::max(rightPeak, right);
        leftSumSq += static_cast<double>(left) * left;
        rightSumSq += static_cast<double>(right) * right;
    }

    diag.leftPeak = leftPeak;
    diag.rightPeak = rightPeak;
    diag.leftRms = frames > 0 ? static_cast<float>(std::sqrt(leftSumSq / frames)) : 0.0f;
    diag.rightRms = frames > 0 ? static_cast<float>(std::sqrt(rightSumSq / frames)) : 0.0f;
}
} // namespace

QByteArray canonicalizeInt16ToMonoStereo(const QByteArray& input,
                                         int inputChannels,
                                         int inputSampleRate,
                                         ChannelMode requestedMode,
                                         AutoState* autoState,
                                         Diagnostics* diagnostics)
{
    const int channels = inputChannels <= 1 ? 1 : 2;
    Diagnostics diag;
    diag.inputChannels = channels;
    diag.inputSampleRate = inputSampleRate;
    if (!boundRealtimeBlock(input, sizeof(int16_t), channels,
                            kMaxRealtimeBlockBytes, diag)) {
        if (diagnostics) {
            *diagnostics = diag;
        }
        return {};
    }

    const qsizetype sampleCount = input.size() / static_cast<qsizetype>(sizeof(int16_t));
    // Narrowing is safe only because boundRealtimeBlock() capped the input at
    // kMaxRealtimeBlockBytes: 256 KiB of Int16 is at most 65,536 frames. The
    // static_assert above is what keeps that true if the cap ever moves.
    const int frames = static_cast<int>(sampleCount / channels);
    diag.frames = frames;

    const auto* src = reinterpret_cast<const int16_t*>(input.constData());
    const qsizetype outputBytes = static_cast<qsizetype>(frames)
        * 2 * static_cast<qsizetype>(sizeof(int16_t));
    QByteArray stereo(outputBytes, Qt::Uninitialized);
    auto* dst = reinterpret_cast<int16_t*>(stereo.data());

    if (channels == 1) {
        analyzeInt16Mono(src, frames, diag);
        diag.selectedMode = ChannelMode::Mono;
        diag.oneSidedStereo = false;
        if (autoState) {
            autoState->reset();
        }

        for (int i = 0; i < frames; ++i) {
            dst[i * 2] = src[i];
            dst[i * 2 + 1] = src[i];
        }
    } else {
        analyzeInt16Stereo(src, frames, diag);
        bool oneSided = false;
        const ChannelMode selected = chooseStereoMode(diag.leftRms,
                                                      diag.rightRms,
                                                      inputSampleRate,
                                                      frames,
                                                      requestedMode,
                                                      autoState,
                                                      &oneSided);
        diag.selectedMode = selected;
        diag.oneSidedStereo = oneSided;

        for (int i = 0; i < frames; ++i) {
            const int16_t left = src[i * 2];
            const int16_t right = src[i * 2 + 1];
            int16_t mono = 0;
            switch (selected) {
            case ChannelMode::Left:
                mono = left;
                break;
            case ChannelMode::Right:
                mono = right;
                break;
            case ChannelMode::Average:
            case ChannelMode::Auto:
            case ChannelMode::Mono:
                mono = averageInt16(left, right);
                break;
            }
            dst[i * 2] = mono;
            dst[i * 2 + 1] = mono;
        }
    }

    if (diagnostics) {
        *diagnostics = diag;
    }
    return stereo;
}

QByteArray canonicalizeFloat32ToMonoStereo(const QByteArray& input,
                                           int inputChannels,
                                           int inputSampleRate,
                                           ChannelMode requestedMode,
                                           AutoState* autoState,
                                           Diagnostics* diagnostics)
{
    const int channels = inputChannels <= 1 ? 1 : 2;
    Diagnostics diag;
    diag.inputChannels = channels;
    diag.inputSampleRate = inputSampleRate;
    if (!boundRealtimeBlock(input, sizeof(float), channels,
                            kMaxRealtimeBlockBytes, diag)) {
        if (diagnostics) {
            *diagnostics = diag;
        }
        return {};
    }

    const qsizetype sampleCount = input.size() / static_cast<qsizetype>(sizeof(float));
    const int frames = static_cast<int>(sampleCount / channels);
    diag.frames = frames;

    const auto* src = reinterpret_cast<const float*>(input.constData());
    const qsizetype outputBytes = static_cast<qsizetype>(frames)
        * 2 * static_cast<qsizetype>(sizeof(float));
    QByteArray stereo(outputBytes, Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(stereo.data());

    if (channels == 1) {
        analyzeFloat32Mono(src, frames, diag);
        diag.selectedMode = ChannelMode::Mono;
        diag.oneSidedStereo = false;
        if (autoState) {
            autoState->reset();
        }

        for (int i = 0; i < frames; ++i) {
            const float mono = finiteOrZero(src[i]);
            dst[i * 2] = mono;
            dst[i * 2 + 1] = mono;
        }
    } else {
        analyzeFloat32Stereo(src, frames, diag);
        bool oneSided = false;
        const ChannelMode selected = chooseStereoMode(diag.leftRms,
                                                      diag.rightRms,
                                                      inputSampleRate,
                                                      frames,
                                                      requestedMode,
                                                      autoState,
                                                      &oneSided);
        diag.selectedMode = selected;
        diag.oneSidedStereo = oneSided;

        for (int i = 0; i < frames; ++i) {
            const float left = finiteOrZero(src[i * 2]);
            const float right = finiteOrZero(src[i * 2 + 1]);
            float mono = 0.0f;
            switch (selected) {
            case ChannelMode::Left:
                mono = left;
                break;
            case ChannelMode::Right:
                mono = right;
                break;
            case ChannelMode::Average:
            case ChannelMode::Auto:
            case ChannelMode::Mono:
                // Average in float. The Int16 sibling rounds here; this route
                // is the reason the whole change exists, so it must not.
                mono = 0.5f * (left + right);
                break;
            }
            dst[i * 2] = mono;
            dst[i * 2 + 1] = mono;
        }
    }

    if (diagnostics) {
        *diagnostics = diag;
    }
    return stereo;
}

QByteArray collapseFloat32ToInt16MonoBigEndian(const QByteArray& input,
                                               int inputChannels,
                                               int inputSampleRate,
                                               ChannelMode requestedMode,
                                               AutoState* autoState,
                                               Diagnostics* diagnostics)
{
    const int channels = inputChannels <= 1 ? 1 : 2;
    Diagnostics diag;
    diag.inputChannels = channels;
    diag.inputSampleRate = inputSampleRate;
    if (!boundRealtimeBlock(input, sizeof(float), channels,
                            kMaxRealtimeFloatBlockBytes, diag)) {
        if (diagnostics) {
            *diagnostics = diag;
        }
        return {};
    }

    const qsizetype sampleCount = input.size() / static_cast<qsizetype>(sizeof(float));
    // Safe for the same reason as the Int16 path: the cap bounds frames well
    // inside int, and float32 frames are larger still.
    const int frames = static_cast<int>(sampleCount / channels);
    diag.frames = frames;

    const auto* src = reinterpret_cast<const float*>(input.constData());
    const qsizetype outputBytes = static_cast<qsizetype>(frames)
        * static_cast<qsizetype>(sizeof(qint16));
    QByteArray mono(outputBytes, Qt::Uninitialized);
    auto* dst = reinterpret_cast<qint16*>(mono.data());

    if (channels == 1) {
        analyzeFloat32Mono(src, frames, diag);
        diag.selectedMode = ChannelMode::Mono;
        diag.oneSidedStereo = false;
        if (autoState) {
            autoState->reset();
        }

        for (int i = 0; i < frames; ++i) {
            dst[i] = qToBigEndian(floatToInt16(src[i]));
        }
    } else {
        analyzeFloat32Stereo(src, frames, diag);
        bool oneSided = false;
        const ChannelMode selected = chooseStereoMode(diag.leftRms,
                                                      diag.rightRms,
                                                      inputSampleRate,
                                                      frames,
                                                      requestedMode,
                                                      autoState,
                                                      &oneSided);
        diag.selectedMode = selected;
        diag.oneSidedStereo = oneSided;

        for (int i = 0; i < frames; ++i) {
            const float left = finiteOrZero(src[i * 2]);
            const float right = finiteOrZero(src[i * 2 + 1]);
            float value = 0.0f;
            switch (selected) {
            case ChannelMode::Left:
                value = left;
                break;
            case ChannelMode::Right:
                value = right;
                break;
            case ChannelMode::Average:
            case ChannelMode::Auto:
            case ChannelMode::Mono:
                value = (left + right) * 0.5f;
                break;
            }
            dst[i] = qToBigEndian(floatToInt16(value));
        }
    }

    if (diagnostics) {
        *diagnostics = diag;
    }
    return mono;
}

LevelBlock measureInt16StereoLevelBlock(const QByteArray& pcm)
{
    LevelBlock block;
    const int sampleCount = pcm.size() / static_cast<int>(sizeof(int16_t));
    if (sampleCount <= 0) {
        return block;
    }

    const auto* src = reinterpret_cast<const int16_t*>(pcm.constData());
    const int stereoFrames = sampleCount / 2;
    for (int i = 0; i < stereoFrames; ++i) {
        const float left = absInt16(src[i * 2]);
        const float right = absInt16(src[i * 2 + 1]);
        const float s = std::max(left, right);
        block.peak = std::max(block.peak, s);
        block.sumSq += static_cast<double>(s) * s;
        ++block.frames;
    }

    if ((sampleCount % 2) != 0) {
        const float s = absInt16(src[sampleCount - 1]);
        block.peak = std::max(block.peak, s);
        block.sumSq += static_cast<double>(s) * s;
        ++block.frames;
    }

    return block;
}

float rmsFromLevelBlock(const LevelBlock& block)
{
    return block.frames > 0 ? static_cast<float>(std::sqrt(block.sumSq / block.frames)) : 0.0f;
}

float dbfs(float linear)
{
    return linear > 1e-10f ? 20.0f * std::log10(linear) : -150.0f;
}

const char* channelModeName(ChannelMode mode)
{
    switch (mode) {
    case ChannelMode::Auto:
        return "Auto";
    case ChannelMode::Left:
        return "Left";
    case ChannelMode::Right:
        return "Right";
    case ChannelMode::Average:
        return "Average";
    case ChannelMode::Mono:
        return "Mono";
    }
    return "Unknown";
}

} // namespace AetherSDR::TxMicChannelNormalizer
