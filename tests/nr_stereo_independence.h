#pragma once

// Shared checks that a client RX noise-reduction wrapper denoises L and R as
// two independent channels, the way RN2 does, rather than through one mono
// analysis applied to both sides.
//
//   leftIgnoresRight / rightIgnoresLeft
//     Two fresh filters see the same signal on one channel and unrelated
//     signals on the other. The shared channel's output must be identical.
//     A mono downmix, a shared mask, or a balance estimated from the pair
//     all make one side's output depend on the other side's input.
//
//   panStepSettles
//     A signal hard-panned left snaps hard right. The left output must be
//     40 dB down within 300 ms, a fixed bound above every method's latency
//     (NNR's ~190 ms on the 24 kHz path is the largest). The mono path used to
//     re-derive the balance with a ~1 s power envelope, so this took about
//     five seconds.
//
//   attenuatesNoise
//     Broadband noise on both channels comes out quieter on each. The three
//     checks above also hold for a filter that does nothing, so this is what
//     tells a silent fallback to passthrough (a failed plan or model on one
//     channel) apart from noise reduction.

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <numbers>

namespace NrStereoIndependence {

using Process = std::function<QByteArray(const QByteArray&)>;
using MakeProcess = std::function<Process()>;

// Deterministic, speech-band content: two tones and a xorshift noise bed.
// `seed` changes the noise; `tone` scales the tones so the two sides can
// differ in both character and level.
inline float testSample(int rate, int frame, std::uint32_t& state,
                        float tone, float noise)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const float white = static_cast<float>(state) / 4294967296.0f - 0.5f;
    const double t = static_cast<double>(frame) / rate;
    const double voiced = 0.6 * std::sin(2.0 * std::numbers::pi * 700.0 * t)
                        + 0.4 * std::sin(2.0 * std::numbers::pi * 1300.0 * t);
    return static_cast<float>(tone * voiced) + noise * white;
}

struct ChannelSpec {
    std::uint32_t seed;
    float tone;
    float noise;
};

inline QByteArray makeStereo(int rate, int frames, ChannelSpec left, ChannelSpec right)
{
    QByteArray pcm(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(pcm.data());
    std::uint32_t leftState = left.seed;
    std::uint32_t rightState = right.seed;
    for (int frame = 0; frame < frames; ++frame) {
        samples[2 * frame] = testSample(rate, frame, leftState, left.tone, left.noise);
        samples[2 * frame + 1] =
            testSample(rate, frame, rightState, right.tone, right.noise);
    }
    return pcm;
}

// Irregular block sizes, the way network audio arrives.
inline QByteArray runBlocks(const Process& process, const QByteArray& input)
{
    constexpr int kPartitions[] = {128, 13, 511, 240, 73, 480};
    constexpr int kFrameBytes = 2 * static_cast<int>(sizeof(float));
    QByteArray output;
    int offset = 0;
    for (int part = 0; offset < input.size(); ++part) {
        const int bytes = std::min<int>(
            kPartitions[part % std::size(kPartitions)] * kFrameBytes,
            input.size() - offset);
        output.append(process(input.mid(offset, bytes)));
        offset += bytes;
    }
    return output;
}

inline bool channelsMatch(const QByteArray& a, const QByteArray& b, int channel)
{
    if (a.size() != b.size()) {
        return false;
    }
    const auto* sa = reinterpret_cast<const float*>(a.constData());
    const auto* sb = reinterpret_cast<const float*>(b.constData());
    const int frames = a.size() / (2 * static_cast<int>(sizeof(float)));
    for (int frame = 0; frame < frames; ++frame) {
        if (sa[2 * frame + channel] != sb[2 * frame + channel]) {
            return false;
        }
    }
    return true;
}

inline double channelRms(const QByteArray& pcm, int channel, int firstFrame, int lastFrame)
{
    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    const int frames = pcm.size() / (2 * static_cast<int>(sizeof(float)));
    firstFrame = std::clamp(firstFrame, 0, frames);
    lastFrame = std::clamp(lastFrame, firstFrame, frames);
    double power = 0.0;
    for (int frame = firstFrame; frame < lastFrame; ++frame) {
        const double value = samples[2 * frame + channel];
        power += value * value;
    }
    const int count = lastFrame - firstFrame;
    return count > 0 ? std::sqrt(power / count) : 0.0;
}

// `shared` is 0 to hold the left input fixed and vary the right, 1 for the
// reverse.
inline bool channelIgnoresOther(const MakeProcess& make, int rate, int shared)
{
    const int frames = rate * 2;
    const ChannelSpec fixed{0x1234567u, 0.25f, 0.05f};
    const ChannelSpec otherA{0x7654321u, 0.05f, 0.20f};
    const ChannelSpec otherB{0x0badf00du, 0.30f, 0.01f};
    const QByteArray inputA = shared == 0
        ? makeStereo(rate, frames, fixed, otherA)
        : makeStereo(rate, frames, otherA, fixed);
    const QByteArray inputB = shared == 0
        ? makeStereo(rate, frames, fixed, otherB)
        : makeStereo(rate, frames, otherB, fixed);
    const QByteArray outputA = runBlocks(make(), inputA);
    const QByteArray outputB = runBlocks(make(), inputB);
    return !outputA.isEmpty() && channelsMatch(outputA, outputB, shared);
}

inline bool leftIgnoresRight(const MakeProcess& make, int rate)
{
    return channelIgnoresOther(make, rate, 0);
}

inline bool rightIgnoresLeft(const MakeProcess& make, int rate)
{
    return channelIgnoresOther(make, rate, 1);
}

// Returns the left output level after the step, relative to its level before,
// in dB. `settleMs` is excluded after the step to cover the filter's latency.
inline double panStepResidualDb(const MakeProcess& make, int rate, int settleMs)
{
    const int half = rate * 3 / 2;
    const ChannelSpec signal{0x2468aceu, 0.25f, 0.05f};
    const ChannelSpec silent{0x1u, 0.0f, 0.0f};
    QByteArray input = makeStereo(rate, half, signal, silent);
    input.append(makeStereo(rate, half, silent, signal));
    const QByteArray output = runBlocks(make(), input);

    const int settle = rate * settleMs / 1000;
    const double before = channelRms(output, 0, half / 2, half);
    const double after = channelRms(output, 0, half + settle, 2 * half);
    if (before <= 0.0) {
        return 0.0;
    }
    return 20.0 * std::log10(std::max(after, 1.0e-12) / before);
}

inline bool panStepSettles(const MakeProcess& make, int rate, int settleMs = 300)
{
    const double residualDb = panStepResidualDb(make, rate, settleMs);
    std::printf("  pan step: left residual %.1f dB after %d ms\n", residualDb, settleMs);
    return residualDb < -40.0;
}

// Stationary white noise on both channels, different on each. Measured over the
// second half so learning periods and startup latency are behind it.
inline bool attenuatesNoise(const MakeProcess& make, int rate, double minReductionDb = 3.0)
{
    const int frames = rate * 3;
    const QByteArray input = makeStereo(rate, frames, {0xa11ceu, 0.0f, 0.2f},
                                        {0xb0bu, 0.0f, 0.2f});
    const QByteArray output = runBlocks(make(), input);
    const int outputFrames = output.size() / (2 * static_cast<int>(sizeof(float)));
    const int last = std::min(frames, outputFrames);
    const int first = rate * 3 / 2;
    bool ok = last > first;
    for (int channel = 0; channel < 2 && ok; ++channel) {
        const double reductionDb = 20.0 * std::log10(
            std::max(channelRms(output, channel, first, last), 1.0e-12)
            / channelRms(input, channel, first, last));
        std::printf("  noise: channel %d changed %.1f dB\n", channel, reductionDb);
        ok = reductionDb <= -minReductionDb;
    }
    return ok;
}

} // namespace NrStereoIndependence
