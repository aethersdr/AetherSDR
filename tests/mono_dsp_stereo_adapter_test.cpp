#include "core/MonoDspStereoAdapter.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

using AetherSDR::MonoDspStereoAdapter;

namespace {

constexpr int kSampleRate = 24000;
constexpr float kPi = 3.14159265358979323846f;

struct Rms {
    double left{0.0};
    double right{0.0};
};

QByteArray makeStereoBlock(int frames, float leftScale, float rightScale)
{
    QByteArray block(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    for (int i = 0; i < frames; ++i) {
        const float carrier =
            std::sin(2.0f * kPi * 733.0f * static_cast<float>(i) / kSampleRate);
        samples[i * 2] = leftScale * carrier;
        samples[i * 2 + 1] = rightScale * carrier;
    }
    return block;
}

QByteArray makeOppositePhaseStereoBlock(int frames, float scale)
{
    QByteArray block(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    for (int i = 0; i < frames; ++i) {
        const float carrier =
            std::sin(2.0f * kPi * 733.0f * static_cast<float>(i) / kSampleRate);
        samples[i * 2] = scale * carrier;
        samples[i * 2 + 1] = -scale * carrier;
    }
    return block;
}

QByteArray makePannedNoisyStereoBlock(int frames, float leftScale, float rightScale)
{
    QByteArray block(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    std::uint32_t state = 0x4d595df4U;
    for (int i = 0; i < frames; ++i) {
        state = state * 1664525U + 1013904223U;
        const float noise = static_cast<float>((state >> 8) & 0xffffU) / 32767.5f - 1.0f;
        const float dry = 0.22f * std::sin(
            2.0f * kPi * 733.0f * static_cast<float>(i) / kSampleRate)
            + 0.12f * noise;
        samples[i * 2] = leftScale * dry;
        samples[i * 2 + 1] = rightScale * dry;
    }
    return block;
}

QByteArray makeDecorrelatedPannedStereoBlock(
    int frames, float leftScale, float rightScale)
{
    QByteArray block(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    std::uint32_t leftState = 0x4d595df4U;
    std::uint32_t rightState = 0x8f7011eeU;
    for (int i = 0; i < frames; ++i) {
        leftState = leftState * 1664525U + 1013904223U;
        rightState = rightState * 22695477U + 1U;
        const float leftNoise =
            static_cast<float>((leftState >> 8) & 0xffffU) / 32767.5f - 1.0f;
        const float rightNoise =
            static_cast<float>((rightState >> 8) & 0xffffU) / 32767.5f - 1.0f;
        const float leftDry = 0.22f * std::sin(
            2.0f * kPi * 733.0f * static_cast<float>(i) / kSampleRate)
            + 0.12f * leftNoise;
        const float rightDry = 0.22f * std::sin(
            2.0f * kPi * 997.0f * static_cast<float>(i) / kSampleRate)
            + 0.12f * rightNoise;
        samples[i * 2] = leftScale * leftDry;
        samples[i * 2 + 1] = rightScale * rightDry;
    }
    return block;
}

QByteArray makeConstantStereoBlock(int frames, float left, float right)
{
    QByteArray block(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    for (int i = 0; i < frames; ++i) {
        samples[i * 2] = left;
        samples[i * 2 + 1] = right;
    }
    return block;
}

std::vector<float> makeProcessedMono(const QByteArray& stereoBlock, float gain)
{
    const auto* samples = reinterpret_cast<const float*>(stereoBlock.constData());
    const int frames = stereoBlock.size() / (2 * static_cast<int>(sizeof(float)));
    std::vector<float> mono(frames);
    for (int i = 0; i < frames; ++i) {
        mono[i] = gain * 0.5f * (samples[i * 2] + samples[i * 2 + 1]);
    }
    return mono;
}

std::vector<float> makeProcessedWaveform(int frames)
{
    std::vector<float> processed(frames);
    for (int i = 0; i < frames; ++i) {
        processed[i] = 0.28f * std::sin(
            2.0f * kPi * 1379.0f * static_cast<float>(i) / kSampleRate);
    }
    return processed;
}

Rms measureRms(const QByteArray& stereoBlock, int discardFrames)
{
    const auto* samples = reinterpret_cast<const float*>(stereoBlock.constData());
    const int frames = stereoBlock.size() / (2 * static_cast<int>(sizeof(float)));
    double leftSq = 0.0;
    double rightSq = 0.0;
    int count = 0;
    for (int i = std::min(discardFrames, frames); i < frames; ++i) {
        leftSq += static_cast<double>(samples[i * 2]) * samples[i * 2];
        rightSq += static_cast<double>(samples[i * 2 + 1]) * samples[i * 2 + 1];
        ++count;
    }

    if (count == 0) {
        return {};
    }
    return {std::sqrt(leftSq / count), std::sqrt(rightSq / count)};
}

bool nearlyEqual(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

bool testPreservesRatioWithSharedGain()
{
    MonoDspStereoAdapter adapter;
    const QByteArray dry = makeStereoBlock(kSampleRate, 0.8f, 0.2f);
    const std::vector<float> processedMono = makeProcessedMono(dry, 0.42f);

    adapter.pushDryStereo(dry);
    const QByteArray out = adapter.takeProcessedMono(
        processedMono.data(), static_cast<int>(processedMono.size()));

    const Rms rms = measureRms(out, kSampleRate / 4);
    const double ratio = rms.left / std::max(rms.right, 1.0e-12);
    if (!nearlyEqual(ratio, 4.0, 0.05)) {
        std::printf("ratio preservation failed: got %.6f expected 4.0\n", ratio);
        return false;
    }
    if (rms.left <= 0.01 || rms.right <= 0.01) {
        std::printf("adapter output unexpectedly quiet: L %.6f R %.6f\n",
                    rms.left,
                    rms.right);
        return false;
    }
    return true;
}

bool testBuffersDryUntilProcessedArrives()
{
    MonoDspStereoAdapter adapter;
    const int frames = 960;
    const QByteArray first = makeStereoBlock(frames, 0.7f, 0.3f);
    const QByteArray second = makeStereoBlock(frames, 0.5f, 0.5f);
    const std::vector<float> firstProcessed = makeProcessedMono(first, 0.35f);

    adapter.pushDryStereo(first);
    adapter.pushDryStereo(second);
    const QByteArray out = adapter.takeProcessedMono(
        firstProcessed.data(), static_cast<int>(firstProcessed.size()));

    if (adapter.bufferedFrames() != frames) {
        std::printf("buffered frame count failed: got %d expected %d\n",
                    adapter.bufferedFrames(),
                    frames);
        return false;
    }

    const Rms rms = measureRms(out, frames / 4);
    const double ratio = rms.left / std::max(rms.right, 1.0e-12);
    if (!nearlyEqual(ratio, 0.7 / 0.3, 0.05)) {
        std::printf("buffer alignment failed: got ratio %.6f\n", ratio);
        return false;
    }
    return true;
}

bool testOppositePhaseStereoUsesCenteredProcessedFallback()
{
    MonoDspStereoAdapter adapter;
    const QByteArray dry = makeOppositePhaseStereoBlock(kSampleRate, 0.6f);
    const std::vector<float> processedMono = makeProcessedWaveform(kSampleRate);

    adapter.pushDryStereo(dry);
    const QByteArray out = adapter.takeProcessedMono(
        processedMono.data(), static_cast<int>(processedMono.size()));

    const auto* samples = reinterpret_cast<const float*>(out.constData());
    for (int i = 0; i < kSampleRate; ++i) {
        if (!nearlyEqual(samples[i * 2], processedMono[i], 1.0e-6)
            || !nearlyEqual(samples[i * 2 + 1], processedMono[i], 1.0e-6)) {
            std::printf("opposite-phase fallback lost processed waveform at frame %d\n", i);
            return false;
        }
    }
    return true;
}

bool testProcessedSilenceRemainsSilent()
{
    MonoDspStereoAdapter adapter;
    const QByteArray dry = makeStereoBlock(kSampleRate * 2, 0.8f, 0.2f);
    const int frames = dry.size() / (2 * static_cast<int>(sizeof(float)));
    std::vector<float> processedMono(frames, 0.0f);

    adapter.pushDryStereo(dry);
    const QByteArray out = adapter.takeProcessedMono(
        processedMono.data(), static_cast<int>(processedMono.size()));

    const auto* samples = reinterpret_cast<const float*>(out.constData());
    for (int i = 0; i < frames * 2; ++i) {
        if (samples[i] != 0.0f) {
            std::printf("processed silence leaked dry audio at sample %d\n", i);
            return false;
        }
    }
    return true;
}

bool testIndependentAdaptersDoNotDrainOtherSource()
{
    MonoDspStereoAdapter sourceA;
    MonoDspStereoAdapter sourceB;
    const int frames = 960;
    const QByteArray dryA = makeStereoBlock(frames, 0.8f, 0.2f);
    const QByteArray dryB = makeStereoBlock(frames, 0.25f, 0.75f);
    const std::vector<float> processedB = makeProcessedMono(dryB, 0.45f);

    sourceA.pushDryStereo(dryA);
    sourceB.pushDryStereo(dryB);
    const QByteArray outB = sourceB.takeProcessedMono(
        processedB.data(), static_cast<int>(processedB.size()));

    if (sourceA.bufferedFrames() != frames) {
        std::printf("source A dry queue was drained: got %d expected %d\n",
                    sourceA.bufferedFrames(),
                    frames);
        return false;
    }

    const Rms rms = measureRms(outB, frames / 4);
    const double ratio = rms.left / std::max(rms.right, 1.0e-12);
    if (!nearlyEqual(ratio, 0.25 / 0.75, 0.05)) {
        std::printf("source B ratio used another source: got %.6f\n", ratio);
        return false;
    }
    return true;
}

bool testDuplicatedMonoUsesProcessedWaveform()
{
    MonoDspStereoAdapter adapter;
    const int frames = 960;
    const QByteArray dry = makeStereoBlock(frames, 0.6f, 0.6f);
    std::vector<float> processed(frames);
    for (int i = 0; i < frames; ++i) {
        processed[i] = 0.2f * std::sin(
            2.0f * kPi * 1379.0f * static_cast<float>(i) / kSampleRate);
    }

    adapter.pushDryStereo(dry);
    const QByteArray out = adapter.takeProcessedMono(processed.data(), frames);
    const auto* samples = reinterpret_cast<const float*>(out.constData());
    for (int i = 0; i < frames; ++i) {
        if (!nearlyEqual(samples[i * 2], processed[i], 1.0e-6)
            || !nearlyEqual(samples[i * 2 + 1], processed[i], 1.0e-6)) {
            std::printf("duplicated mono discarded processed waveform at frame %d\n", i);
            return false;
        }
    }
    return true;
}

bool testPannedDryDoesNotReplaceProcessedWaveform()
{
    MonoDspStereoAdapter adapter;
    const int frames = kSampleRate;
    const QByteArray dry = makePannedNoisyStereoBlock(frames, 0.8f, 0.2f);
    const std::vector<float> processed = makeProcessedWaveform(frames);

    adapter.pushDryStereo(dry);
    const QByteArray out = adapter.takeProcessedMono(processed.data(), frames);
    const auto* samples = reinterpret_cast<const float*>(out.constData());
    for (int i = frames / 4; i < frames; ++i) {
        const float expectedLeft = processed[i] * 1.6f;
        const float expectedRight = processed[i] * 0.4f;
        if (!nearlyEqual(samples[i * 2], expectedLeft, 1.0e-5)
            || !nearlyEqual(samples[i * 2 + 1], expectedRight, 1.0e-5)) {
            std::printf("panned dry replaced or modulated processed waveform at frame %d\n", i);
            return false;
        }
    }
    return true;
}

bool testQuietChannelDoesNotLatchMuted()
{
    MonoDspStereoAdapter adapter;
    const int frames = kSampleRate;
    const QByteArray dry = makeStereoBlock(frames, 0.02f, 0.001f);
    const std::vector<float> processed(frames, 0.1f);

    adapter.pushDryStereo(dry);
    const QByteArray out = adapter.takeProcessedMono(processed.data(), frames);
    const auto* samples = reinterpret_cast<const float*>(out.constData());
    const float expectedLeft = 2.0f * 0.02f / 0.021f;
    const float expectedRight = 2.0f * 0.001f / 0.021f;
    for (int i = frames / 2; i < frames; ++i) {
        if (!nearlyEqual(samples[i * 2], 0.1f * expectedLeft, 1.0e-4)
            || !nearlyEqual(samples[i * 2 + 1], 0.1f * expectedRight, 1.0e-4)) {
            std::printf("quiet balance latched a channel at frame %d: L %.6f R %.6f\n",
                        i,
                        samples[i * 2],
                        samples[i * 2 + 1]);
            return false;
        }
    }
    return true;
}

bool testBalanceTransitionUsesSlowEnvelope()
{
    MonoDspStereoAdapter adapter;
    const int settleFrames = kSampleRate * 3;
    const int transitionFrames = kSampleRate * 2;
    const QByteArray initial =
        makeDecorrelatedPannedStereoBlock(settleFrames, 0.8f, 0.2f);
    const QByteArray swapped =
        makeDecorrelatedPannedStereoBlock(transitionFrames, 0.2f, 0.8f);
    const std::vector<float> settleProcessed(settleFrames, 0.1f);
    const std::vector<float> transitionProcessed(transitionFrames, 0.1f);

    adapter.pushDryStereo(initial);
    adapter.takeProcessedMono(settleProcessed.data(), settleFrames);
    adapter.pushDryStereo(swapped);
    const QByteArray out = adapter.takeProcessedMono(
        transitionProcessed.data(), transitionFrames);
    const auto* samples = reinterpret_cast<const float*>(out.constData());

    constexpr int kEarlyFrame = 1000;
    const float earlyLeftBalance = samples[kEarlyFrame * 2] / 0.1f;
    const float earlyRightBalance = samples[kEarlyFrame * 2 + 1] / 0.1f;
    if (earlyLeftBalance < 1.35f || earlyRightBalance > 0.65f) {
        std::printf("balance transition tracked too quickly: L %.6f R %.6f\n",
                    earlyLeftBalance,
                    earlyRightBalance);
        return false;
    }

    const int finalFrame = transitionFrames - 1;
    const float finalLeftBalance = samples[finalFrame * 2] / 0.1f;
    const float finalRightBalance = samples[finalFrame * 2 + 1] / 0.1f;
    if (finalLeftBalance > 0.8f || finalRightBalance < 1.2f) {
        std::printf("balance transition tracked too slowly: L %.6f R %.6f\n",
                    finalLeftBalance,
                    finalRightBalance);
        return false;
    }
    return true;
}

bool testDryUnderrunHoldsLastBalance()
{
    MonoDspStereoAdapter adapter;
    const QByteArray panned = makePannedNoisyStereoBlock(kSampleRate, 0.8f, 0.2f);
    const std::vector<float> processed(kSampleRate, 0.1f);

    adapter.pushDryStereo(panned);
    adapter.takeProcessedMono(processed.data(), kSampleRate);
    const QByteArray out = adapter.takeProcessedMono(processed.data(), kSampleRate);
    const auto* samples = reinterpret_cast<const float*>(out.constData());
    for (int i = 0; i < kSampleRate; ++i) {
        if (!nearlyEqual(samples[i * 2], 0.16f, 1.0e-5)
            || !nearlyEqual(samples[i * 2 + 1], 0.04f, 1.0e-5)) {
            std::printf("dry underrun centered the held balance at frame %d\n", i);
            return false;
        }
    }
    return true;
}

bool testMonoCancellationHoldsLastBalance()
{
    MonoDspStereoAdapter adapter;
    const QByteArray panned = makePannedNoisyStereoBlock(kSampleRate, 0.8f, 0.2f);
    const QByteArray cancelled = makeOppositePhaseStereoBlock(kSampleRate, 0.6f);
    const std::vector<float> processed = makeProcessedWaveform(kSampleRate);

    adapter.pushDryStereo(panned);
    adapter.takeProcessedMono(processed.data(), kSampleRate);
    adapter.pushDryStereo(cancelled);
    const QByteArray out = adapter.takeProcessedMono(processed.data(), kSampleRate);
    const auto* samples = reinterpret_cast<const float*>(out.constData());
    for (int i = kSampleRate / 4; i < kSampleRate; ++i) {
        const float expectedLeft = processed[i] * 1.6f;
        const float expectedRight = processed[i] * 0.4f;
        if (!nearlyEqual(samples[i * 2], expectedLeft, 2.0e-3)
            || !nearlyEqual(samples[i * 2 + 1], expectedRight, 2.0e-3)) {
            std::printf("mono-cancellation fallback changed the established balance at frame %d\n", i);
            return false;
        }
    }

    adapter.pushDryStereo(panned);
    const QByteArray recovered = adapter.takeProcessedMono(processed.data(), kSampleRate);
    const auto* recoveredSamples = reinterpret_cast<const float*>(recovered.constData());
    for (int i = 0; i < kSampleRate / 4; ++i) {
        const float expectedLeft = processed[i] * 1.6f;
        const float expectedRight = processed[i] * 0.4f;
        if (!nearlyEqual(recoveredSamples[i * 2], expectedLeft, 2.0e-3)
            || !nearlyEqual(recoveredSamples[i * 2 + 1], expectedRight, 2.0e-3)) {
            std::printf("mono-cancellation recovery changed the established balance at frame %d\n", i);
            return false;
        }
    }
    return true;
}

bool testProcessingLatencyRetainsDryTimeline()
{
    constexpr int latencyFrames = 4;
    MonoDspStereoAdapter adapter(latencyFrames);
    const QByteArray dry = makeConstantStereoBlock(8, 0.8f, 0.2f);
    const std::vector<float> processed(8, 0.0f);

    adapter.pushDryStereo(dry);
    const QByteArray out = adapter.takeProcessedMono(processed.data(), 8);
    const auto* samples = reinterpret_cast<const float*>(out.constData());
    for (int i = 0; i < latencyFrames; ++i) {
        if (samples[i * 2] != 0.0f || samples[i * 2 + 1] != 0.0f) {
            std::printf("latency priming emitted dry audio at frame %d\n", i);
            return false;
        }
    }
    if (adapter.bufferedFrames() != latencyFrames) {
        std::printf("latency pairing consumed wrong dry count: got %d expected %d\n",
                    adapter.bufferedFrames(),
                    latencyFrames);
        return false;
    }
    return true;
}

bool testOverflowClearsInsteadOfMisaligning()
{
    MonoDspStereoAdapter adapter;
    adapter.pushDryStereo(makeConstantStereoBlock(kSampleRate * 6, 0.8f, 0.2f));
    if (adapter.bufferedFrames() != 0) {
        std::printf("overflow retained a misaligned dry timeline: %d frames\n",
                    adapter.bufferedFrames());
        return false;
    }
    return true;
}

} // namespace

int main()
{
    if (!testPreservesRatioWithSharedGain()) {
        return 1;
    }
    if (!testBuffersDryUntilProcessedArrives()) {
        return 1;
    }
    if (!testPannedDryDoesNotReplaceProcessedWaveform()) {
        return 1;
    }
    if (!testQuietChannelDoesNotLatchMuted()) {
        return 1;
    }
    if (!testBalanceTransitionUsesSlowEnvelope()) {
        return 1;
    }
    if (!testDryUnderrunHoldsLastBalance()) {
        return 1;
    }
    if (!testOppositePhaseStereoUsesCenteredProcessedFallback()) {
        return 1;
    }
    if (!testProcessedSilenceRemainsSilent()) {
        return 1;
    }
    if (!testIndependentAdaptersDoNotDrainOtherSource()) {
        return 1;
    }
    if (!testDuplicatedMonoUsesProcessedWaveform()) {
        return 1;
    }
    if (!testMonoCancellationHoldsLastBalance()) {
        return 1;
    }
    if (!testProcessingLatencyRetainsDryTimeline()) {
        return 1;
    }
    if (!testOverflowClearsInsteadOfMisaligning()) {
        return 1;
    }
    std::printf("mono_dsp_stereo_adapter_test passed\n");
    return 0;
}
