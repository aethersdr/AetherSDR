#include "core/RNNoiseFilter.h"

#include <QByteArray>

#include <cmath>
#include <cstdio>

using AetherSDR::RNNoiseFilter;

namespace {

constexpr int kBlockFrames = 480;
constexpr int kSampleRate = 24000;
constexpr float kPi = 3.14159265358979323846f;

QByteArray makeUnequalStereoBlock(int blockIndex)
{
    QByteArray block(
        kBlockFrames * 2 * static_cast<int>(sizeof(float)),
        Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    for (int i = 0; i < kBlockFrames; ++i) {
        const int frame = blockIndex * kBlockFrames + i;
        const float seconds = static_cast<float>(frame) / kSampleRate;
        const float envelope = 0.55f + 0.35f * std::sin(2.0f * kPi * 3.0f * seconds);
        const float signal = envelope * (
            0.45f * std::sin(2.0f * kPi * 180.0f * seconds)
            + 0.25f * std::sin(2.0f * kPi * 440.0f * seconds)
            + 0.15f * std::sin(2.0f * kPi * 1000.0f * seconds));
        samples[i * 2] = signal;
        samples[i * 2 + 1] = 0.25f * signal;
    }
    return block;
}

bool testProcessedMonoOutputIsDuplicated()
{
    RNNoiseFilter filter(RNNoiseFilter::OutputMode::ProcessedMono);
    if (!filter.isValid()) {
        std::printf("RNNoise initialization failed\n");
        return false;
    }

    bool heardProcessedAudio = false;
    for (int blockIndex = 0; blockIndex < 150; ++blockIndex) {
        const QByteArray output = filter.process(makeUnequalStereoBlock(blockIndex));
        const auto* samples = reinterpret_cast<const float*>(output.constData());
        for (int i = 0; i < kBlockFrames; ++i) {
            const float left = samples[i * 2];
            const float right = samples[i * 2 + 1];
            if (std::abs(left - right) > 1.0e-6f) {
                std::printf(
                    "processed-mono output restored dry stereo at block %d frame %d\n",
                    blockIndex,
                    i);
                return false;
            }
            heardProcessedAudio = heardProcessedAudio || std::abs(left) > 1.0e-5f;
        }
    }

    if (!heardProcessedAudio) {
        std::printf("processed-mono output remained silent after startup\n");
        return false;
    }
    return true;
}

} // namespace

int main()
{
    if (!testProcessedMonoOutputIsDuplicated()) {
        return 1;
    }
    std::printf("rnnoise_filter_test passed\n");
    return 0;
}
