#include "core/RNNoiseFilter.h"
#include "rnnoise.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using AetherSDR::RNNoiseFilter;

namespace {

constexpr int kBlockFrames = 480;
constexpr int kSampleRate = 24000;
constexpr int kRnNoiseLatencyFrames = 480;
constexpr int kNoisePadFrames = 2 * kSampleRate;
constexpr int kWindowFrames = kSampleRate / 10;
constexpr float kPi = 3.14159265358979323846f;

// The dry mix Rn2SettingsModel can be raised to. Not RN2's default — the
// default is 0 (full suppression) — so the tests that exercise it set it
// explicitly rather than depending on a constant buried in the filter.
constexpr float kTestDryMix = 0.15f;

uint32_t readLe32(const unsigned char* bytes)
{
    return static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16)
        | (static_cast<uint32_t>(bytes[3]) << 24);
}

std::vector<float> readDemoVoice()
{
    const QString path = QStringLiteral(":/demo_voice.wav");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::printf("could not open RN2 test voice fixture: %s\n"
                    "(build issue, not a regression — check the "
                    "rnnoise_filter_test.qrc resource)\n",
                    path.toUtf8().constData());
        return {};
    }

    const QByteArray data = file.readAll();
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.constData());
    const std::size_t byteCount = static_cast<std::size_t>(data.size());
    if (byteCount < 44 || std::memcmp(bytes, "RIFF", 4) != 0
        || std::memcmp(bytes + 8, "WAVE", 4) != 0) {
        std::printf("RN2 test voice fixture is not a RIFF/WAVE file\n");
        return {};
    }

    std::size_t offset = 12;
    while (offset + 8 <= byteCount) {
        const uint32_t chunkSize = readLe32(bytes + offset + 4);
        const std::size_t payloadOffset = offset + 8;
        if (payloadOffset + chunkSize > byteCount) {
            return {};
        }
        if (std::memcmp(bytes + offset, "data", 4) == 0) {
            std::vector<float> samples(chunkSize / 2);
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const uint16_t raw =
                    static_cast<uint16_t>(bytes[payloadOffset + i * 2])
                    | (static_cast<uint16_t>(bytes[payloadOffset + i * 2 + 1]) << 8);
                samples[i] = static_cast<float>(static_cast<int16_t>(raw)) / 32768.0f;
            }
            return samples;
        }
        offset = payloadOffset + chunkSize + (chunkSize & 1U);
    }
    return {};
}

float rms(const std::vector<float>& samples, int startFrame, int frames)
{
    double sum = 0.0;
    for (int i = 0; i < frames; ++i) {
        const double sample = samples[startFrame + i];
        sum += sample * sample;
    }
    return static_cast<float>(std::sqrt(sum / std::max(frames, 1)));
}

float median(std::vector<float> values)
{
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

// Goertzel magnitude of one bin — used to track a narrowband probe tone that
// the speech fixture does not overlap.
double toneLevel(const std::vector<float>& samples, int startFrame, int frames,
                 double hz)
{
    const double w = 2.0 * static_cast<double>(kPi) * hz / kSampleRate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (int i = 0; i < frames; ++i) {
        const double s0 = samples[startFrame + i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w);
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / frames;
}

QByteArray processInBlocks(RNNoiseFilter& filter, const QByteArray& input,
                           const std::vector<int>& blockFrames)
{
    constexpr int kStereoFrameBytes = 2 * static_cast<int>(sizeof(float));
    const int totalFrames = input.size() / kStereoFrameBytes;
    QByteArray output;
    output.reserve(input.size());
    int offsetFrames = 0;
    int blockIndex = 0;
    while (offsetFrames < totalFrames) {
        const int requestedFrames = blockFrames[blockIndex % blockFrames.size()];
        const int frames = std::min(requestedFrames, totalFrames - offsetFrames);
        output.append(filter.process(input.mid(offsetFrames * kStereoFrameBytes,
                                               frames * kStereoFrameBytes)));
        offsetFrames += frames;
        ++blockIndex;
    }
    return output;
}

bool processNativeInBlocks(RNNoiseFilter& filter, const QByteArray& input,
                           const std::vector<int>& blockFrames,
                           QByteArray& output)
{
    constexpr int kStereoFrameBytes = 2 * static_cast<int>(sizeof(float));
    const int totalFrames = input.size() / kStereoFrameBytes;
    output.resize(0);
    output.reserve(input.size());
    QByteArray blockOutput;
    int offsetFrames = 0;
    int blockIndex = 0;
    while (offsetFrames < totalFrames) {
        const int requestedFrames = blockFrames[blockIndex % blockFrames.size()];
        const int frames = std::min(requestedFrames, totalFrames - offsetFrames);
        const QByteArray inputBlock = input.mid(
            offsetFrames * kStereoFrameBytes, frames * kStereoFrameBytes);
        const int outputFrames = filter.process48kStereo(inputBlock, blockOutput);
        if (outputFrames != frames || blockOutput.size() != inputBlock.size()) {
            return false;
        }
        output.append(blockOutput);
        offsetFrames += frames;
        ++blockIndex;
    }
    return true;
}

QByteArray makeStereoSine(int sampleRate, int frames, float rightGain)
{
    QByteArray block(frames * 2 * static_cast<int>(sizeof(float)),
                     Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    for (int frame = 0; frame < frames; ++frame) {
        const float seconds = static_cast<float>(frame) / sampleRate;
        const float left = 0.45f * std::sin(2.0f * kPi * 430.0f * seconds);
        samples[frame * 2] = left;
        samples[frame * 2 + 1] = rightGain * left;
    }
    return block;
}

bool hasAudibleLeftChannel(const QByteArray& samples, int startFrame, int frames)
{
    const auto* interleaved = reinterpret_cast<const float*>(samples.constData());
    for (int frame = startFrame; frame < startFrame + frames; ++frame) {
        if (std::abs(interleaved[frame * 2]) > 1.0e-4f) {
            return true;
        }
    }
    return false;
}

int firstAudibleLeftFrame(const QByteArray& samples)
{
    const auto* interleaved = reinterpret_cast<const float*>(samples.constData());
    const int frames = samples.size() / (2 * static_cast<int>(sizeof(float)));
    for (int frame = 0; frame < frames; ++frame) {
        if (std::abs(interleaved[frame * 2]) > 1.0e-4f) {
            return frame;
        }
    }
    return -1;
}

QByteArray makeUnequalStereoBlock(int blockIndex)
{
    QByteArray block(kBlockFrames * 2 * static_cast<int>(sizeof(float)),
                     Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    for (int i = 0; i < kBlockFrames; ++i) {
        const int frame = blockIndex * kBlockFrames + i;
        const float seconds = static_cast<float>(frame) / kSampleRate;
        const float envelope = 0.55f + 0.35f * std::sin(2.0f * kPi * 3.0f * seconds);
        const float signal = envelope
            * (0.45f * std::sin(2.0f * kPi * 180.0f * seconds)
               + 0.25f * std::sin(2.0f * kPi * 440.0f * seconds)
               + 0.15f * std::sin(2.0f * kPi * 1000.0f * seconds));
        samples[i * 2] = signal;
        samples[i * 2 + 1] = 0.25f * signal;
    }
    return block;
}

bool testSpectralDryMixUsesOneSynthesisTimeline()
{
    using StatePtr = std::unique_ptr<DenoiseState, decltype(&rnnoise_destroy)>;
    StatePtr legacy(rnnoise_create(nullptr), &rnnoise_destroy);
    StatePtr zeroDry(rnnoise_create(nullptr), &rnnoise_destroy);
    StatePtr fullDry(rnnoise_create(nullptr), &rnnoise_destroy);
    StatePtr mixed(rnnoise_create(nullptr), &rnnoise_destroy);
    if (!legacy || !zeroDry || !fullDry || !mixed) {
        std::printf("spectral dry-mix RNNoise initialization failed\n");
        return false;
    }

    constexpr int kFrames = 80;
    std::array<float, 480> input{};
    std::array<float, 480> legacyOutput{};
    std::array<float, 480> zeroDryOutput{};
    std::array<float, 480> fullDryOutput{};
    std::array<float, 480> mixedOutput{};
    float maxMixError = 0.0f;
    uint32_t randomState = 0xbb67ae85U;

    for (int frame = 0; frame < kFrames; ++frame) {
        for (int i = 0; i < static_cast<int>(input.size()); ++i) {
            randomState ^= randomState << 13;
            randomState ^= randomState >> 17;
            randomState ^= randomState << 5;
            const float noise =
                (static_cast<float>(randomState & 0xffffU) / 32767.5f - 1.0f) * 900.0f;
            const int sample = frame * static_cast<int>(input.size()) + i;
            const float seconds = static_cast<float>(sample) / 48000.0f;
            input[i] = noise + 6500.0f * std::sin(2.0f * kPi * 730.0f * seconds)
                + 2800.0f * std::sin(2.0f * kPi * 1210.0f * seconds);
        }

        rnnoise_process_frame(legacy.get(), legacyOutput.data(), input.data());
        rnnoise_process_frame_with_dry_mix(zeroDry.get(), zeroDryOutput.data(),
                                           input.data(), 0.0f);
        rnnoise_process_frame_with_dry_mix(fullDry.get(), fullDryOutput.data(),
                                           input.data(), 1.0f);
        rnnoise_process_frame_with_dry_mix(mixed.get(), mixedOutput.data(),
                                           input.data(), kTestDryMix);

        for (int i = 0; i < static_cast<int>(input.size()); ++i) {
            if (legacyOutput[i] != zeroDryOutput[i]) {
                std::printf(
                    "zero-dry RNNoise changed legacy output at frame %d sample %d\n",
                    frame, i);
                return false;
            }
            const float expected = (1.0f - kTestDryMix) * legacyOutput[i]
                + kTestDryMix * fullDryOutput[i];
            maxMixError = std::max(maxMixError, std::abs(mixedOutput[i] - expected));
        }
    }

    if (maxMixError > 0.01f) {
        std::printf("spectral dry mix left the shared synthesis timeline: %.6f\n",
                    maxMixError);
        return false;
    }

    std::printf("RN2 spectral dry-mix maximum linearity error: %.6f\n", maxMixError);
    return true;
}

bool testDryMixDefaultsToFullSuppressionAndClamps()
{
    RNNoiseFilter filter;
    if (filter.dryMix() != 0.0f) {
        std::printf("RN2 dry mix did not default to full suppression: %.6f\n",
                    filter.dryMix());
        return false;
    }

    // The settings model clamps too, but the filter is the last line: a stored
    // value from a newer schema must not be able to invert the wet/dry blend.
    filter.setDryMix(-1.0f);
    if (filter.dryMix() != 0.0f) {
        std::printf("RN2 dry mix accepted a negative value: %.6f\n",
                    filter.dryMix());
        return false;
    }
    filter.setDryMix(5.0f);
    if (filter.dryMix() != 1.0f) {
        std::printf("RN2 dry mix accepted a value above 1: %.6f\n", filter.dryMix());
        return false;
    }
    filter.setDryMix(std::nanf(""));
    if (filter.dryMix() != 0.0f) {
        std::printf("RN2 dry mix accepted NaN: %.6f\n", filter.dryMix());
        return false;
    }
    return true;
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
                    blockIndex, i);
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

bool testIrregularPartitionsPreserveRnnoiseLatency()
{
    constexpr int kTotalFrames = 2 * kSampleRate;
    constexpr int kLegacyFirstAudibleMin = 4500;
    constexpr int kLegacyFirstAudibleMax = 4525;
    constexpr int kNativeFirstAudibleMin = 1850;
    constexpr int kNativeFirstAudibleMax = 1880;
    const std::vector<int> partitions{73, 211, 17, 604, 91};

    RNNoiseFilter legacyFilter;
    legacyFilter.setDryMix(1.0f);
    const QByteArray legacyInput = makeStereoSine(kSampleRate, kTotalFrames, 0.25f);
    const QByteArray legacyOutput = processInBlocks(
        legacyFilter, legacyInput, partitions);
    const int legacyFirstAudible = firstAudibleLeftFrame(legacyOutput);
    if (legacyOutput.size() != legacyInput.size()
        || legacyFirstAudible < kLegacyFirstAudibleMin
        || legacyFirstAudible > kLegacyFirstAudibleMax) {
        std::printf("legacy RN2 irregular partitions changed output size or latency "
                    "(first audible frame %d)\n", legacyFirstAudible);
        return false;
    }

    RNNoiseFilter nativeFilter(
        RNNoiseFilter::OutputMode::PreserveRxStereo,
        RNNoiseFilter::RateDomain::Native48k);
    nativeFilter.setDryMix(1.0f);
    const QByteArray nativeInput = makeStereoSine(48000, kTotalFrames, 0.25f);
    QByteArray nativeOutput;
    const bool nativeProcessed = processNativeInBlocks(
        nativeFilter, nativeInput, partitions, nativeOutput);
    const int nativeFirstAudible = firstAudibleLeftFrame(nativeOutput);
    if (!nativeProcessed
        || nativeOutput.size() != nativeInput.size()
        || nativeFirstAudible < kNativeFirstAudibleMin
        || nativeFirstAudible > kNativeFirstAudibleMax) {
        std::printf("native RN2 irregular partitions changed output size or latency "
                    "(first audible frame %d)\n", nativeFirstAudible);
        return false;
    }
    std::printf("RN2 irregular partition latency: legacy=%d native=%d frames\n",
                legacyFirstAudible, nativeFirstAudible);
    return true;
}

bool testNative48PreservesRxStereoIndependence()
{
    constexpr int kTotalFrames = 4 * kRnNoiseLatencyFrames;
    RNNoiseFilter filter(
        RNNoiseFilter::OutputMode::PreserveRxStereo,
        RNNoiseFilter::RateDomain::Native48k);
    filter.setDryMix(1.0f);

    const QByteArray input = makeStereoSine(48000, kTotalFrames, 0.0f);
    QByteArray output;
    if (!processNativeInBlocks(filter, input, {91, 389, 27, 613}, output)
        || output.size() != input.size()) {
        std::printf("native RX stereo path did not preserve its output contract\n");
        return false;
    }

    const auto* samples = reinterpret_cast<const float*>(output.constData());
    for (int frame = kRnNoiseLatencyFrames; frame < kTotalFrames; ++frame) {
        if (std::abs(samples[frame * 2 + 1]) > 1.0e-6f) {
            std::printf("native RX stereo path mixed left into right at frame %d\n", frame);
            return false;
        }
    }
    if (!hasAudibleLeftChannel(
            output, kRnNoiseLatencyFrames,
            kTotalFrames - kRnNoiseLatencyFrames)) {
        std::printf("native RX stereo path did not retain the left channel\n");
        return false;
    }
    return true;
}

bool testNative48ProcessedMonoIrregularPartitionsPreserveFifo()
{
    constexpr int kTotalFrames = 2 * kSampleRate;
    constexpr int kComparedFrames = 4096;
    const QByteArray input = makeStereoSine(48000, kTotalFrames, -0.35f);

    RNNoiseFilter alignedFilter(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);
    alignedFilter.setDryMix(1.0f);
    QByteArray alignedOutput;
    if (!processNativeInBlocks(alignedFilter, input, {480}, alignedOutput)) {
        std::printf("native processed-mono aligned reference failed\n");
        return false;
    }

    RNNoiseFilter irregularFilter(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);
    irregularFilter.setDryMix(1.0f);
    QByteArray irregularOutput;
    if (!processNativeInBlocks(
            irregularFilter, input, {73, 211, 17, 604, 91}, irregularOutput)
        || irregularOutput.size() != input.size()) {
        std::printf("native processed-mono irregular output contract failed\n");
        return false;
    }

    const auto* irregularSamples =
        reinterpret_cast<const float*>(irregularOutput.constData());
    for (int frame = 0; frame < kTotalFrames; ++frame) {
        if (std::abs(irregularSamples[frame * 2]
                     - irregularSamples[frame * 2 + 1]) > 1.0e-6f) {
            std::printf("native processed-mono output diverged at frame %d\n", frame);
            return false;
        }
    }

    const int alignedStart = firstAudibleLeftFrame(alignedOutput);
    const int irregularStart = firstAudibleLeftFrame(irregularOutput);
    if (alignedStart < 0 || irregularStart < 0
        || alignedStart + kComparedFrames > kTotalFrames
        || irregularStart + kComparedFrames > kTotalFrames) {
        std::printf("native processed-mono FIFO comparison lacked audible output\n");
        return false;
    }

    const auto* alignedSamples =
        reinterpret_cast<const float*>(alignedOutput.constData());
    for (int frame = 0; frame < kComparedFrames; ++frame) {
        const float aligned = alignedSamples[(alignedStart + frame) * 2];
        const float irregular = irregularSamples[(irregularStart + frame) * 2];
        if (std::abs(aligned - irregular) > 1.0e-6f) {
            std::printf("native processed-mono FIFO changed at compared frame %d\n",
                        frame);
            return false;
        }
    }
    return true;
}

// ── The #4689 regression guard ──────────────────────────────────────────────
//
// The reported symptom is that the noise floor RISES WITH SPEECH and drops
// again between phrases. Asserting where the floor sits does not catch that;
// asserting that it does not MOVE does. A steady narrowband tone the speech
// fixture does not overlap makes the floor directly measurable: mix a 6 kHz
// probe into a binaural input, then compare its level in speech windows with
// its level in the gaps.
//
// Deriving one gain from the L/R downmix and re-applying it to channels that
// are not proportional to that downmix scores ~15x here. Per-channel RNNoise
// scores ~1.0x. Both cases below fail on the pre-#4689 filter.
struct PumpingMetrics {
    double inputSpeech{0.0};
    double outputSpeech{0.0};
    double outputGap{0.0};
};

bool measureProbeToneModulation(float dryMix, PumpingMetrics& metrics)
{
    constexpr int kBinauralDelayFrames = 12;
    constexpr double kProbeHz = 6000.0;
    constexpr float kProbeAmplitude = 0.05f;

    // Only the speech region is measured here, so this run uses a short lead-in
    // (enough for RNNoise to settle on the noise floor) and a trimmed fixture
    // rather than the full-length timeline the noise-tail tests below need.
    constexpr int kLeadInFrames = kSampleRate;
    const std::vector<float> fullVoice = readDemoVoice();
    if (fullVoice.empty()) {
        return false;
    }
    const std::vector<float> demoVoice(
        fullVoice.begin(),
        fullVoice.begin()
            + std::min<std::size_t>(fullVoice.size(), 5 * kSampleRate));

    const int totalFrames = kLeadInFrames + static_cast<int>(demoVoice.size());
    std::vector<float> mono(totalFrames, 0.0f);
    uint32_t randomState = 0x6a09e667U;
    for (int i = 0; i < totalFrames; ++i) {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        const float noise =
            (static_cast<float>(randomState & 0xffffU) / 32767.5f - 1.0f) * 0.010f;
        const int voiceIndex = i - kLeadInFrames;
        const float speech =
            voiceIndex >= 0 && voiceIndex < static_cast<int>(demoVoice.size())
            ? demoVoice[voiceIndex] * 0.72f
            : 0.0f;
        const float probe = kProbeAmplitude
            * std::sin(2.0f * kPi * static_cast<float>(kProbeHz)
                       * static_cast<float>(i) / kSampleRate);
        mono[i] = std::clamp(speech + noise + probe, -1.0f, 1.0f);
    }

    QByteArray input(totalFrames * 2 * static_cast<int>(sizeof(float)),
                     Qt::Uninitialized);
    auto* inputSamples = reinterpret_cast<float*>(input.data());
    for (int i = 0; i < totalFrames; ++i) {
        inputSamples[i * 2] = mono[i];
        inputSamples[i * 2 + 1] =
            i >= kBinauralDelayFrames ? mono[i - kBinauralDelayFrames] : 0.0f;
    }

    RNNoiseFilter filter;
    if (!filter.isValid()) {
        std::printf("probe-tone RNNoise initialization failed\n");
        return false;
    }
    filter.setDryMix(dryMix);
    const QByteArray output = processInBlocks(filter, input, {137, 911, 480, 53, 1200});
    if (output.size() != input.size()) {
        std::printf("probe-tone RN2 output size mismatch\n");
        return false;
    }

    const auto* outputSamples = reinterpret_cast<const float*>(output.constData());
    std::vector<float> left(totalFrames);
    for (int i = 0; i < totalFrames; ++i) {
        left[i] = outputSamples[i * 2];
    }

    std::vector<float> inputSpeechLevels;
    std::vector<float> outputSpeechLevels;
    std::vector<float> outputGapLevels;
    for (int offset = kLeadInFrames;
         offset + kWindowFrames <= totalFrames;
         offset += kWindowFrames) {
        const float voiceLevel = rms(demoVoice, offset - kLeadInFrames, kWindowFrames);
        const auto outputLevel = static_cast<float>(
            toneLevel(left, offset, kWindowFrames, kProbeHz));
        if (voiceLevel > 0.08f) {
            inputSpeechLevels.push_back(static_cast<float>(
                toneLevel(mono, offset, kWindowFrames, kProbeHz)));
            outputSpeechLevels.push_back(outputLevel);
        } else if (voiceLevel < 0.005f) {
            outputGapLevels.push_back(outputLevel);
        }
    }

    if (inputSpeechLevels.empty() || outputGapLevels.empty()) {
        std::printf("probe-tone fixture yielded no usable speech/gap windows\n");
        return false;
    }

    metrics.inputSpeech = median(inputSpeechLevels);
    metrics.outputSpeech = median(outputSpeechLevels);
    metrics.outputGap = median(outputGapLevels);
    return true;
}

bool testNoiseFloorDoesNotBreatheWithSpeech()
{
    // Case A — RN2 as shipped (dry mix 0). A steady tone must stay suppressed
    // while speech is present. The pre-fix filter leaks ~55% of it.
    PumpingMetrics full;
    if (!measureProbeToneModulation(0.0f, full)) {
        return false;
    }
    const double leak = full.outputSpeech / std::max(full.inputSpeech, 1.0e-12);
    if (leak > 0.05) {
        std::printf("RN2 leaked the steady noise floor during speech: "
                    "%.6f of input (limit 0.05)\n",
                    leak);
        return false;
    }

    // Case B — with a dry floor configured, the floor must be a FLOOR: the
    // same level during speech as between phrases. A downmix-derived gain
    // modulates it instead (~4x even with an identical floor blended in).
    PumpingMetrics floored;
    if (!measureProbeToneModulation(kTestDryMix, floored)) {
        return false;
    }
    const double modulation = floored.outputSpeech / std::max(floored.outputGap, 1.0e-12);
    if (modulation > 1.5) {
        std::printf("RN2 noise floor breathed with speech: %.3fx (limit 1.50x)\n",
                    modulation);
        return false;
    }

    std::printf("RN2 pumping metrics: suppressed leak=%.6f of input, "
                "floored modulation=%.3fx\n",
                leak, modulation);
    return true;
}

bool testStereoChannelsStaySynchronizedWithConstantDryFloor()
{
    const std::vector<float> demoVoice = readDemoVoice();
    if (demoVoice.empty()) {
        return false;
    }

    const int totalFrames =
        kNoisePadFrames + static_cast<int>(demoVoice.size()) + kNoisePadFrames;
    std::vector<float> voice(totalFrames, 0.0f);
    std::copy(demoVoice.begin(), demoVoice.end(), voice.begin() + kNoisePadFrames);

    QByteArray input(totalFrames * 2 * static_cast<int>(sizeof(float)),
                     Qt::Uninitialized);
    auto* inputSamples = reinterpret_cast<float*>(input.data());
    std::vector<float> rawMono(totalFrames);
    uint32_t randomState = 0x9e3779b9U;
    for (int i = 0; i < totalFrames; ++i) {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        const float noise =
            (static_cast<float>(randomState & 0xffffU) / 32767.5f - 1.0f) * 0.035f;
        const float mixture = std::clamp(voice[i] * 0.72f + noise, -1.0f, 1.0f);
        inputSamples[i * 2] = mixture;
        inputSamples[i * 2 + 1] = 0.25f * mixture;
        rawMono[i] = 0.625f * mixture;
    }

    RNNoiseFilter filter;
    if (!filter.isValid()) {
        std::printf("stereo RNNoise initialization failed\n");
        return false;
    }
    filter.setDryMix(kTestDryMix);
    const QByteArray output = processInBlocks(filter, input, {137, 911, 480, 53, 1200});
    if (output.size() != input.size()) {
        std::printf("stereo RNNoise output size mismatch: got %lld expected %lld\n",
                    static_cast<long long>(output.size()),
                    static_cast<long long>(input.size()));
        return false;
    }

    const auto* outputSamples = reinterpret_cast<const float*>(output.constData());
    std::vector<float> left(totalFrames);
    std::vector<float> right(totalFrames);
    std::vector<float> outputMono(totalFrames);
    for (int i = 0; i < totalFrames; ++i) {
        left[i] = outputSamples[i * 2];
        right[i] = outputSamples[i * 2 + 1];
        outputMono[i] = 0.5f * (left[i] + right[i]);
    }

    // The configured dry mix should show up as exactly that much of the raw
    // signal surviving where RNNoise suppresses everything else.
    std::vector<float> noiseGains;
    const int noiseStart =
        kNoisePadFrames + static_cast<int>(demoVoice.size()) + kSampleRate / 2;
    for (int offset = noiseStart; offset + kWindowFrames <= totalFrames;
         offset += kWindowFrames) {
        const float rawLevel = rms(rawMono, offset, kWindowFrames);
        const float outputLevel = rms(outputMono, offset, kWindowFrames);
        noiseGains.push_back(outputLevel / std::max(rawLevel, 1.0e-9f));
    }
    const float noiseGain = median(noiseGains);
    if (std::abs(noiseGain - kTestDryMix) > 0.02f) {
        std::printf("panned RN2 dry floor drifted: gain %.6f, configured %.2f\n",
                    noiseGain, kTestDryMix);
        return false;
    }

    std::vector<float> speechRatios;
    double sameFrameProduct = 0.0;
    double leftPower = 0.0;
    double rightPower = 0.0;
    const int speechStart = kNoisePadFrames + kSampleRate / 2;
    const int speechEnd = kNoisePadFrames + static_cast<int>(demoVoice.size());
    for (int offset = speechStart; offset + kWindowFrames <= speechEnd;
         offset += kWindowFrames) {
        if (rms(voice, offset, kWindowFrames) < 0.04f) {
            continue;
        }
        const float leftLevel = rms(left, offset, kWindowFrames);
        const float rightLevel = rms(right, offset, kWindowFrames);
        speechRatios.push_back(leftLevel / std::max(rightLevel, 1.0e-9f));
        for (int i = 0; i < kWindowFrames; ++i) {
            const double leftSample = left[offset + i];
            const double rightSample = right[offset + i];
            sameFrameProduct += leftSample * rightSample;
            leftPower += leftSample * leftSample;
            rightPower += rightSample * rightSample;
        }
    }

    const float speechRatio = median(speechRatios);
    if (std::abs(speechRatio - 4.0f) >= 0.08f) {
        std::printf("panned RN2 stereo ratio drifted: got %.6f expected 4.0\n",
                    speechRatio);
        return false;
    }
    const double channelCorrelation =
        sameFrameProduct / std::sqrt(std::max(leftPower * rightPower, 1.0e-18));
    if (channelCorrelation < 0.999) {
        std::printf("RN2 channel timelines diverged: correlation %.9f\n",
                    channelCorrelation);
        return false;
    }

    filter.reset();
    if (!filter.isValid()) {
        std::printf("stereo RNNoise reset failed\n");
        return false;
    }
    // reset() must not disturb the configured dry mix — it re-primes the DSP
    // state, it does not reload settings.
    if (filter.dryMix() != kTestDryMix) {
        std::printf("RN2 reset discarded the configured dry mix: %.6f\n",
                    filter.dryMix());
        return false;
    }
    const QByteArray afterReset = filter.process(
        input.left(kBlockFrames * 2 * static_cast<int>(sizeof(float))));
    const auto* resetSamples = reinterpret_cast<const float*>(afterReset.constData());
    for (int i = 0; i < kRnNoiseLatencyFrames; ++i) {
        if (resetSamples[i * 2] != 0.0f || resetSamples[i * 2 + 1] != 0.0f) {
            std::printf(
                "RN2 reset did not re-prime both channels together at frame %d\n", i);
            return false;
        }
    }

    std::printf("RN2 stereo regression metrics: noiseGain=%.6f ratio=%.6f corr=%.9f\n",
                noiseGain, speechRatio, channelCorrelation);
    return true;
}

bool testBinauralPhaseDifferenceSurvivesDenoising()
{
    constexpr int kBinauralDelayFrames = 12;
    const std::vector<float> demoVoice = readDemoVoice();
    if (demoVoice.empty()) {
        return false;
    }

    const int totalFrames =
        kNoisePadFrames + static_cast<int>(demoVoice.size()) + kNoisePadFrames;
    std::vector<float> mono(totalFrames, 0.0f);
    uint32_t randomState = 0x6a09e667U;
    for (int i = 0; i < totalFrames; ++i) {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        const float noise =
            (static_cast<float>(randomState & 0xffffU) / 32767.5f - 1.0f) * 0.035f;
        const int voiceIndex = i - kNoisePadFrames;
        const float speech =
            voiceIndex >= 0 && voiceIndex < static_cast<int>(demoVoice.size())
            ? demoVoice[voiceIndex] * 0.72f
            : 0.0f;
        mono[i] = std::clamp(speech + noise, -1.0f, 1.0f);
    }

    QByteArray input(totalFrames * 2 * static_cast<int>(sizeof(float)),
                     Qt::Uninitialized);
    auto* inputSamples = reinterpret_cast<float*>(input.data());
    for (int i = 0; i < totalFrames; ++i) {
        inputSamples[i * 2] = mono[i];
        inputSamples[i * 2 + 1] =
            i >= kBinauralDelayFrames ? mono[i - kBinauralDelayFrames] : 0.0f;
    }

    RNNoiseFilter filter;
    if (!filter.isValid()) {
        std::printf("binaural RNNoise initialization failed\n");
        return false;
    }
    filter.setDryMix(kTestDryMix);
    const QByteArray output = processInBlocks(filter, input, {73, 480, 997, 211});
    if (output.size() != input.size()) {
        std::printf("binaural RNNoise output size mismatch\n");
        return false;
    }

    const auto* outputSamples = reinterpret_cast<const float*>(output.constData());
    std::vector<float> left(totalFrames);
    std::vector<float> right(totalFrames);
    for (int i = 0; i < totalFrames; ++i) {
        left[i] = outputSamples[i * 2];
        right[i] = outputSamples[i * 2 + 1];
    }

    const int speechStart = kNoisePadFrames + kSampleRate / 2;
    const int speechFrames =
        std::max(0, static_cast<int>(demoVoice.size()) - kSampleRate);
    double differencePower = 0.0;
    double outputPower = 0.0;
    for (int i = 0; i < speechFrames; ++i) {
        const double leftSample = left[speechStart + i];
        const double rightSample = right[speechStart + i];
        const double difference = leftSample - rightSample;
        differencePower += difference * difference;
        outputPower += 0.5 * (leftSample * leftSample + rightSample * rightSample);
    }
    const double normalizedDifference =
        std::sqrt(differencePower / std::max(outputPower, 1.0e-18));
    if (normalizedDifference < 0.1) {
        std::printf("RN2 collapsed binaural channels: normalized difference %.6f\n",
                    normalizedDifference);
        return false;
    }

    const int noiseStart =
        kNoisePadFrames + static_cast<int>(demoVoice.size()) + kSampleRate / 2;
    const int noiseFrames = totalFrames - noiseStart;
    const float leftNoiseGain = rms(left, noiseStart, noiseFrames)
        / std::max(rms(mono, noiseStart, noiseFrames), 1.0e-9f);
    const float rightNoiseGain = rms(right, noiseStart, noiseFrames)
        / std::max(rms(mono, noiseStart - kBinauralDelayFrames, noiseFrames), 1.0e-9f);
    if (std::abs(leftNoiseGain - kTestDryMix) > 0.02f
        || std::abs(rightNoiseGain - kTestDryMix) > 0.02f) {
        std::printf("binaural RN2 dry floor drifted: L %.6f R %.6f, configured %.2f\n",
                    leftNoiseGain, rightNoiseGain, kTestDryMix);
        return false;
    }

    std::printf("RN2 binaural regression metrics: difference=%.6f "
                "noiseGainL=%.6f noiseGainR=%.6f\n",
                normalizedDifference, leftNoiseGain, rightNoiseGain);
    return true;
}

bool testNative48ReusableOutputPreservesCapacity()
{
    RNNoiseFilter filter(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);
    QByteArray input(
        kBlockFrames * 2 * static_cast<int>(sizeof(float)), '\0');
    QByteArray output;
    output.reserve(input.size() * 4);
    const qsizetype reservedCapacity = output.capacity();
    const int outputFrames = filter.process48kStereo(input, output);

    if (outputFrames != kBlockFrames || output.size() != input.size()
        || output.capacity() < reservedCapacity) {
        std::printf("native-48 reusable output lost capacity: "
                    "frames=%d bytes=%lld reserved=%lld capacity=%lld\n",
                    outputFrames,
                    static_cast<long long>(output.size()),
                    static_cast<long long>(reservedCapacity),
                    static_cast<long long>(output.capacity()));
        return false;
    }
    return true;
}

bool testNative48EntryPointRejectsLegacyRateDomain()
{
    RNNoiseFilter filter(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Legacy24k);
    QByteArray input(
        kBlockFrames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(input.data());
    for (int frame = 0; frame < kBlockFrames; ++frame) {
        samples[frame * 2] = 0.25f;
        samples[frame * 2 + 1] = -0.125f;
    }

    QByteArray output;
    const int outputFrames = filter.process48kStereo(input, output);
    if (outputFrames != kBlockFrames || output != input) {
        std::printf("legacy rate domain did not pass through native-48 input: "
                    "frames=%d bytes=%lld\n",
                    outputFrames, static_cast<long long>(output.size()));
        return false;
    }
    return true;
}

} // namespace

int main()
{
    if (!testSpectralDryMixUsesOneSynthesisTimeline()
        || !testDryMixDefaultsToFullSuppressionAndClamps()
        || !testProcessedMonoOutputIsDuplicated()
        || !testIrregularPartitionsPreserveRnnoiseLatency()
        || !testNative48PreservesRxStereoIndependence()
        || !testNative48ProcessedMonoIrregularPartitionsPreserveFifo()
        || !testNoiseFloorDoesNotBreatheWithSpeech()
        || !testStereoChannelsStaySynchronizedWithConstantDryFloor()
        || !testBinauralPhaseDifferenceSurvivesDenoising()
        || !testNative48ReusableOutputPreservesCapacity()
        || !testNative48EntryPointRejectsLegacyRateDomain()) {
        return 1;
    }
    std::printf("rnnoise_filter_test passed\n");
    return 0;
}
