#include "core/ClientTube.h"
#include "core/ClientGate.h"
#include "core/RNNoiseFilter.h"
#include "core/Resampler.h"
#include "core/TxVoiceProcessor.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using AetherSDR::ClientTube;
using AetherSDR::ClientGate;
using AetherSDR::RNNoiseFilter;
using AetherSDR::Resampler;
using AetherSDR::TxVoiceProcessor;

namespace AetherSDR {

class TxVoiceProcessorTestAccess
{
public:
    static int reconcileEgressFrameCounts(
        TxVoiceProcessor& processor, int leftFrames, int rightFrames)
    {
        return processor.reconcileEgressFrameCounts(leftFrames, rightFrames);
    }

    static void dirtyEgressResamplerHistories(TxVoiceProcessor& processor)
    {
        std::vector<float> left(480, 0.25f);
        std::vector<float> right(960, -0.25f);
        QByteArray discarded;
        processor.m_outputLeftResampler->process(
            left.data(), static_cast<int>(left.size()), discarded);
        processor.m_outputRightResampler->process(
            right.data(), static_cast<int>(right.size()), discarded);
    }
};

} // namespace AetherSDR

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-68s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

QByteArray makeCanonicalTone(int frames, int sampleRate, float frequencyHz)
{
    QByteArray result(
        frames * 2 * static_cast<int>(sizeof(int16_t)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<int16_t*>(result.data());
    constexpr double kTwoPi = 6.28318530717958647692;
    for (int frame = 0; frame < frames; ++frame) {
        const float value = 0.25f * static_cast<float>(
            std::sin(kTwoPi * frequencyHz * frame / sampleRate));
        const int16_t quantized = static_cast<int16_t>(value * 32767.0f);
        samples[frame * 2] = quantized;
        samples[frame * 2 + 1] = quantized;
    }
    return result;
}

std::vector<float> makeFloatStereoTone(
    int frames, int sampleRate, float frequencyHz)
{
    std::vector<float> result(static_cast<size_t>(frames * 2));
    constexpr double kTwoPi = 6.28318530717958647692;
    for (int frame = 0; frame < frames; ++frame) {
        const float sample = 0.25f * static_cast<float>(
            std::sin(kTwoPi * frequencyHz * frame / sampleRate));
        result[static_cast<size_t>(frame * 2)] = sample;
        result[static_cast<size_t>(frame * 2 + 1)] = sample;
    }
    return result;
}

double leftChannelRms(const QByteArray& floatStereo, int skipFrames)
{
    const auto* samples = reinterpret_cast<const float*>(
        floatStereo.constData());
    const int frames = floatStereo.size()
        / (2 * static_cast<int>(sizeof(float)));
    if (!samples || frames <= skipFrames) {
        return 0.0;
    }

    double sumSquares = 0.0;
    for (int frame = skipFrames; frame < frames; ++frame) {
        const double sample = samples[frame * 2];
        sumSquares += sample * sample;
    }
    return std::sqrt(sumSquares / (frames - skipFrames));
}

double transportToneRms(float frequencyHz)
{
    constexpr int kInputFrames = 48000;
    constexpr int kSettlingOutputFrames = 4000;
    const std::vector<float> input = makeFloatStereoTone(
        kInputFrames, TxVoiceProcessor::kDspRate, frequencyHz);

    TxVoiceProcessor processor;
    QByteArray transportOutput;
    if (!processor.prepare(TxVoiceProcessor::kDspRate, kInputFrames)
        || !processor.processFloat48(
            input.data(), kInputFrames, transportOutput)) {
        return 0.0;
    }
    return leftChannelRms(
        processor.transportFloat32Stereo(), kSettlingOutputFrames);
}

double capturedTransportToneRms(int inputRate, float frequencyHz)
{
    const int inputFrames = inputRate * 2;
    TxVoiceProcessor processor;
    QByteArray inputOutput = makeCanonicalTone(
        inputFrames, inputRate, frequencyHz);
    if (!processor.prepare(inputRate, inputFrames)
        || !processor.processCapturedInt16(inputOutput)) {
        return 0.0;
    }

    // Measure the final second so both SRCs are in steady state and every
    // integer-Hz test tone spans an exact number of cycles.
    return leftChannelRms(
        processor.transportFloat32Stereo(),
        TxVoiceProcessor::kTransportRate);
}

double capturedNormalizedToneRms(int inputRate, float frequencyHz)
{
    const int inputFrames = inputRate * 2;
    TxVoiceProcessor processor;
    processor.setMeasurementCaptureEnabled(true);
    QByteArray inputOutput = makeCanonicalTone(
        inputFrames, inputRate, frequencyHz);
    if (!processor.prepare(inputRate, inputFrames)
        || !processor.processCapturedInt16(inputOutput)) {
        return 0.0;
    }

    return leftChannelRms(
        processor.normalizedFloat48Stereo(), TxVoiceProcessor::kDspRate);
}

double leftChannelToneMagnitude(const QByteArray& floatStereo,
                                int sampleRate,
                                float frequencyHz,
                                int startFrame,
                                int analysisFrames)
{
    const auto* samples = reinterpret_cast<const float*>(
        floatStereo.constData());
    const int availableFrames = floatStereo.size()
        / (2 * static_cast<int>(sizeof(float)));
    if (!samples || startFrame < 0 || analysisFrames <= 0
        || startFrame + analysisFrames > availableFrames) {
        return 0.0;
    }

    constexpr double kTwoPi = 6.28318530717958647692;
    double inPhase = 0.0;
    double quadrature = 0.0;
    for (int frame = 0; frame < analysisFrames; ++frame) {
        const double phase = kTwoPi * frequencyHz * frame / sampleRate;
        const double sample = samples[(startFrame + frame) * 2];
        inPhase += sample * std::cos(phase);
        quadrature += sample * std::sin(phase);
    }
    return 2.0 * std::hypot(inPhase, quadrature) / analysisFrames;
}

bool finiteFloatBuffer(const QByteArray& bytes)
{
    const auto* samples = reinterpret_cast<const float*>(bytes.constData());
    const int count = bytes.size() / static_cast<int>(sizeof(float));
    for (int index = 0; index < count; ++index) {
        if (!std::isfinite(samples[index])) {
            return false;
        }
    }
    return true;
}

bool duplicatedStereo(const QByteArray& bytes, bool floatSamples)
{
    const int sampleBytes = floatSamples
        ? static_cast<int>(sizeof(float))
        : static_cast<int>(sizeof(int16_t));
    const int frames = bytes.size() / (2 * sampleBytes);
    if (floatSamples) {
        const auto* samples = reinterpret_cast<const float*>(bytes.constData());
        for (int frame = 0; frame < frames; ++frame) {
            if (samples[frame * 2] != samples[frame * 2 + 1]) {
                return false;
            }
        }
    } else {
        const auto* samples = reinterpret_cast<const int16_t*>(bytes.constData());
        for (int frame = 0; frame < frames; ++frame) {
            if (samples[frame * 2] != samples[frame * 2 + 1]) {
                return false;
            }
        }
    }
    return true;
}

uint64_t packedSingleStage(TxVoiceProcessor::Stage stage)
{
    return static_cast<uint64_t>(stage);
}

void testFixedRateContract()
{
    report("DSP rate is pinned to 48 kHz", TxVoiceProcessor::kDspRate == 48000);
    report("transport rate remains 24 kHz",
           TxVoiceProcessor::kTransportRate == 24000);
    report("TX voice SRC transition profile is 12 percent",
           TxVoiceProcessor::kVoiceSrcTransitionBandPercent == 12.0);
}

void testVoiceSrcBandwidthAndAliasRejection()
{
    const double referenceRms = transportToneRms(1000.0f);
    const double tenKhzRms = transportToneRms(10000.0f);
    const double tenKhzGainDb = 20.0 * std::log10(
        std::max(tenKhzRms / referenceRms, 1.0e-15));

    report("voice SRC remains within 0.1 dB through 10 kHz",
           referenceRms > 0.0 && std::abs(tenKhzGainDb) <= 0.1,
           "gainDb=" + std::to_string(tenKhzGainDb));

    double worstAliasDb = -300.0;
    for (const float frequencyHz : {
             14000.0f, 16000.0f, 18000.0f, 20000.0f, 22000.0f}) {
        const double aliasRms = transportToneRms(frequencyHz);
        const double aliasDb = 20.0 * std::log10(
            std::max(aliasRms / referenceRms, 1.0e-15));
        worstAliasDb = std::max(worstAliasDb, aliasDb);
    }
    report("aliases folding into 0-10 kHz remain below -100 dB",
           referenceRms > 0.0 && worstAliasDb <= -100.0,
           "worstAliasDb=" + std::to_string(worstAliasDb));
}

void testCapturedRateSrcBandwidthAndAliasRejection()
{
    const double reference24 = capturedTransportToneRms(24000, 1000.0f);
    const double tenKhz24 = capturedTransportToneRms(24000, 10000.0f);
    const double gain24Db = 20.0 * std::log10(
        std::max(tenKhz24 / reference24, 1.0e-15));
    report("24 -> 48 -> 24 path remains within 0.1 dB through 10 kHz",
           reference24 > 0.0 && std::abs(gain24Db) <= 0.1,
           "gainDb=" + std::to_string(gain24Db));

    const double reference441 = capturedTransportToneRms(44100, 1000.0f);
    const double tenKhz441 = capturedTransportToneRms(44100, 10000.0f);
    const double gain441Db = 20.0 * std::log10(
        std::max(tenKhz441 / reference441, 1.0e-15));
    report("44.1 -> 48 -> 24 path remains within 0.1 dB through 10 kHz",
           reference441 > 0.0 && std::abs(gain441Db) <= 0.1,
           "gainDb=" + std::to_string(gain441Db));

    const double normalizedReference441 =
        capturedNormalizedToneRms(44100, 1000.0f);
    const double normalizedTenKhz441 =
        capturedNormalizedToneRms(44100, 10000.0f);
    const double normalizedGain441Db = 20.0 * std::log10(
        std::max(normalizedTenKhz441 / normalizedReference441, 1.0e-15));
    report("44.1 -> 48 normalization remains within 0.1 dB through 10 kHz",
           normalizedReference441 > 0.0
               && std::abs(normalizedGain441Db) <= 0.1,
           "gainDb=" + std::to_string(normalizedGain441Db));

    // A 6 kHz tone sampled at 24 kHz has an interpolation image at 18 kHz
    // after expansion to 48 kHz. Use the exact four-sample-period tone so
    // int16 source quantization cannot set the rejection floor.
    constexpr int kInputRate = 24000;
    constexpr int kInputFrames = kInputRate * 2;
    constexpr int kAnalysisFrames = TxVoiceProcessor::kDspRate;
    TxVoiceProcessor imageProcessor;
    imageProcessor.setMeasurementCaptureEnabled(true);
    QByteArray imageInputOutput = makeCanonicalTone(
        kInputFrames, kInputRate, 6000.0f);
    const bool imageProcessed = imageProcessor.prepare(
        kInputRate, kInputFrames)
        && imageProcessor.processCapturedInt16(imageInputOutput);
    const QByteArray& normalized = imageProcessor.normalizedFloat48Stereo();
    const int normalizedFrames = normalized.size()
        / (2 * static_cast<int>(sizeof(float)));
    const int analysisStart = normalizedFrames - kAnalysisFrames;
    const double fundamentalMagnitude = leftChannelToneMagnitude(
        normalized, TxVoiceProcessor::kDspRate, 6000.0f,
        analysisStart, kAnalysisFrames);
    const double imageMagnitude = leftChannelToneMagnitude(
        normalized, TxVoiceProcessor::kDspRate, 18000.0f,
        analysisStart, kAnalysisFrames);
    const double imageDb = 20.0 * std::log10(
        std::max(imageMagnitude / fundamentalMagnitude, 1.0e-15));
    report("24 -> 48 interpolation image remains below -100 dB",
           imageProcessed && fundamentalMagnitude > 0.0 && imageDb <= -100.0,
           "imageDb=" + std::to_string(imageDb));

    // 14.7 kHz is exactly one third of 44.1 kHz, giving an int16-periodic
    // stop-band probe without broadband quantization residue. If it survived
    // the serial SRCs it would fold to 9.3 kHz at the 24 kHz transport rate.
    const double alias441 = capturedTransportToneRms(44100, 14700.0f);
    const double alias441Db = 20.0 * std::log10(
        std::max(alias441 / reference441, 1.0e-15));
    report("44.1 -> 48 -> 24 alias probe remains below -100 dB",
           reference441 > 0.0 && alias441Db <= -100.0,
           "aliasDb=" + std::to_string(alias441Db));
}

void testVoiceSrcLatencyBudgets()
{
    struct ExpectedLatency {
        int inputRate;
        int frames48;
        int budgetFrames48;
    };
    constexpr ExpectedLatency kExpected[] = {
        {48000, 394, 960},
        {44100, 615, 960},
        {24000, 788, 960},
        // Low-rate capture remains supported for macOS Bluetooth/HFP and the
        // Windows fallback ladder. These ratios require longer interpolation
        // filters than the common 24/44.1/48 kHz device rates.
        {16000, 1240, 1440},
        {8000, 2104, 2160},
    };

    bool exact = true;
    bool commonRatesWithinTwentyMs = true;
    bool withinRateSpecificBudgets = true;
    std::string detail;
    for (const ExpectedLatency expected : kExpected) {
        TxVoiceProcessor processor;
        const bool prepared = processor.prepare(expected.inputRate, 1024);
        const int frames = processor.latencyFrames();
        exact = prepared && frames == expected.frames48 && exact;
        if (expected.inputRate >= 24000) {
            commonRatesWithinTwentyMs = prepared && frames <= 960
                && commonRatesWithinTwentyMs;
        }
        withinRateSpecificBudgets = prepared
            && frames <= expected.budgetFrames48
            && withinRateSpecificBudgets;
        detail += std::to_string(expected.inputRate) + "Hz="
            + std::to_string(frames) + " ";
    }

    report("SRC group delay is reported for every capture rate", exact, detail);
    report("24/44.1/48 kHz serial SRC delay remains below 20 ms",
           commonRatesWithinTwentyMs,
           detail);
    report("supported low-rate SRC delays remain within documented budgets",
           withinRateSpecificBudgets,
           detail);
}

void testReusableBuffersPreserveCapacity()
{
    constexpr int kFrames = 480;
    std::vector<float> input(kFrames, 0.0f);
    Resampler resampler(48000, 24000, kFrames,
                        TxVoiceProcessor::kVoiceSrcTransitionBandPercent);
    QByteArray output;
    output.reserve(16384);
    const qsizetype reservedCapacity = output.capacity();
    const int outputFrames = resampler.process(input.data(), kFrames, output);

    report("reusable SRC output preserves caller reservation",
           outputFrames == kFrames / 2
               && output.capacity() >= reservedCapacity,
           "reserved=" + std::to_string(reservedCapacity)
               + " capacity=" + std::to_string(output.capacity()));

    constexpr int kPreparedFrames = 1024;
    TxVoiceProcessor processor;
    const bool prepared = processor.prepare(48000, kPreparedFrames);
    const qsizetype normalizedCapacity =
        processor.normalizedFloat48Stereo().capacity();
    const qsizetype postStripCapacity =
        processor.postChannelStripFloat48Stereo().capacity();
    const qsizetype transportFloatCapacity =
        processor.transportFloat32Stereo().capacity();
    const bool reservationsSurvivedPrepare = prepared
        && normalizedCapacity >= kPreparedFrames * 2 * static_cast<int>(sizeof(float))
        && postStripCapacity >= kPreparedFrames * 2 * static_cast<int>(sizeof(float))
        && transportFloatCapacity > 0;

    processor.reset();
    report("TX prepare and reset preserve realtime buffer reservations",
           reservationsSurvivedPrepare
               && processor.normalizedFloat48Stereo().capacity()
                   >= normalizedCapacity
               && processor.postChannelStripFloat48Stereo().capacity()
                   >= postStripCapacity
               && processor.transportFloat32Stereo().capacity()
                   >= transportFloatCapacity);
}

void testCapturedTransportUsesCallerOwnedStorage()
{
    constexpr int kFrames = 480;
    TxVoiceProcessor processor;
    const bool prepared = processor.prepare(48000, kFrames);

    QByteArray firstBlock = makeCanonicalTone(kFrames, 48000, 997.0f);
    const char* const firstInputStorage = firstBlock.constData();
    const bool firstProcessed = processor.processCapturedInt16(firstBlock);
    const bool firstStorageReused = firstBlock.constData() == firstInputStorage;

    // Model the queued monitor/recorder consumers that retain the completed
    // block after the audio callback returns.
    const QByteArray retainedFirstBlock = firstBlock;

    QByteArray secondBlock = makeCanonicalTone(kFrames, 48000, 1201.0f);
    const char* const secondInputStorage = secondBlock.constData();
    const bool secondProcessed = processor.processCapturedInt16(secondBlock);
    const bool secondStorageReused = secondBlock.constData() == secondInputStorage;

    report("captured transport reuses unique caller-owned input storage",
           prepared && firstProcessed && secondProcessed
               && firstStorageReused && secondStorageReused);
    report("retained transport output does not affect the next callback buffer",
           !retainedFirstBlock.isEmpty()
               && retainedFirstBlock.constData() != secondBlock.constData());
}

void testResamplerResetRestoresFreshState()
{
    constexpr int kFrames = 960;
    const std::vector<float> history = makeFloatStereoTone(
        kFrames, TxVoiceProcessor::kDspRate, 317.0f);
    const std::vector<float> probe = makeFloatStereoTone(
        kFrames, TxVoiceProcessor::kDspRate, 997.0f);
    std::vector<float> historyMono(kFrames);
    std::vector<float> probeMono(kFrames);
    for (int frame = 0; frame < kFrames; ++frame) {
        historyMono[static_cast<size_t>(frame)] =
            history[static_cast<size_t>(frame * 2)];
        probeMono[static_cast<size_t>(frame)] =
            probe[static_cast<size_t>(frame * 2)];
    }

    Resampler resetInstance(48000, 24000, kFrames);
    resetInstance.processMonoToStereo(historyMono.data(), kFrames);
    resetInstance.reset();
    const QByteArray afterReset = resetInstance.processMonoToStereo(
        probeMono.data(), kFrames);

    Resampler freshInstance(48000, 24000, kFrames);
    const QByteArray freshOutput = freshInstance.processMonoToStereo(
        probeMono.data(), kFrames);
    constexpr int kStereoFloatFrameBytes =
        2 * static_cast<int>(sizeof(float));
    const int resetFrames = afterReset.size() / kStereoFloatFrameBytes;
    const int freshFrames = freshOutput.size() / kStereoFloatFrameBytes;

    report("resampler reset restores fresh deterministic stream state",
           resetFrames == freshFrames && afterReset == freshOutput,
           "resetFrames=" + std::to_string(resetFrames)
               + " freshFrames=" + std::to_string(freshFrames));
}

void testStereoEgressMismatchSalvagesAndRealigns()
{
    constexpr int kFrames = 480;
    TxVoiceProcessor recovered;
    recovered.prepare(48000, kFrames);
    AetherSDR::TxVoiceProcessorTestAccess::dirtyEgressResamplerHistories(
        recovered);
    const int commonFrames =
        AetherSDR::TxVoiceProcessorTestAccess::reconcileEgressFrameCounts(
            recovered, 240, 239);

    report("stereo egress mismatch preserves the common aligned prefix",
           commonFrames == 239,
           "frames=" + std::to_string(commonFrames));
    report("stereo egress mismatch defers expensive SRC recovery",
           recovered.egressRecoveryPending());

    // AudioEngine services this request on its event loop after the realtime
    // callback returns. The headless test performs the same control-boundary
    // action explicitly.
    recovered.recoverEgressAfterMismatch();
    report("deferred stereo egress recovery clears the request",
           !recovered.egressRecoveryPending());

    const std::vector<float> probe = makeFloatStereoTone(
        kFrames, TxVoiceProcessor::kDspRate, 997.0f);
    QByteArray recoveredOutput;
    const bool recoveredProcessed = recovered.processFloat48(
        probe.data(), kFrames, recoveredOutput);

    TxVoiceProcessor fresh;
    fresh.prepare(48000, kFrames);
    QByteArray freshOutput;
    const bool freshProcessed = fresh.processFloat48(
        probe.data(), kFrames, freshOutput);

    report("stereo egress mismatch resets both channels to fresh alignment",
           recoveredProcessed && freshProcessed
               && recovered.transportFloat32Stereo()
                   == fresh.transportFloat32Stereo());
}

void test48kBypassAndMeasurementBoundaries()
{
    TxVoiceProcessor processor;
    processor.setMeasurementCaptureEnabled(true);
    const bool prepared = processor.prepare(48000, 1024);
    QByteArray transportOutput = makeCanonicalTone(480, 48000, 1000.0f);
    const bool processed = processor.processCapturedInt16(transportOutput);

    report("48 kHz processor prepares", prepared);
    report("48 kHz block processes", processed);
    report("normalized measurement contains 480 stereo float frames",
           processor.normalizedFloat48Stereo().size()
               == 480 * 2 * static_cast<int>(sizeof(float)));
    report("post-strip measurement contains 480 stereo float frames",
           processor.postChannelStripFloat48Stereo().size()
               == 480 * 2 * static_cast<int>(sizeof(float)));
    report("egress contains 240 stereo float frames",
           processor.transportFloat32Stereo().size()
               == 240 * 2 * static_cast<int>(sizeof(float)));
    report("egress contains 240 stereo int16 frames",
           transportOutput.size()
               == 240 * 2 * static_cast<int>(sizeof(int16_t)));
    report("bypass output remains finite",
           finiteFloatBuffer(processor.transportFloat32Stereo()));
    report("mono voice remains duplicated stereo through egress",
           duplicatedStereo(processor.transportFloat32Stereo(), true)
               && duplicatedStereo(transportOutput, false));
}

void testDeviceRateNormalization()
{
    TxVoiceProcessor processor;
    processor.setMeasurementCaptureEnabled(true);
    const bool prepared = processor.prepare(44100, 1024);
    QByteArray transportOutput = makeCanonicalTone(441, 44100, 1000.0f);
    const bool processed = processor.processCapturedInt16(transportOutput);
    const int normalizedFrames = processor.normalizedFloat48Stereo().size()
        / (2 * static_cast<int>(sizeof(float)));
    const int transportFrames = transportOutput.size()
        / (2 * static_cast<int>(sizeof(int16_t)));

    report("44.1 kHz capture rate prepares", prepared);
    report("44.1 kHz capture block processes", processed);
    report("10 ms at 44.1 kHz normalizes to 480 frames",
           normalizedFrames == 480,
           "frames=" + std::to_string(normalizedFrames));
    report("normalized 10 ms block exits as 240 transport frames",
           transportFrames == 240,
           "frames=" + std::to_string(transportFrames));
}

void testFloat48OfflineEntryAvoidsInputQuantization()
{
    std::vector<float> input(480 * 2);
    for (int frame = 0; frame < 480; ++frame) {
        const float sample = 1.0e-6f * static_cast<float>(frame + 1);
        input[static_cast<size_t>(frame * 2)] = sample;
        input[static_cast<size_t>(frame * 2 + 1)] = -sample;
    }

    TxVoiceProcessor processor;
    processor.setMeasurementCaptureEnabled(true);
    processor.prepare(48000, 480);
    QByteArray transportOutput;
    const bool processed = processor.processFloat48(
        input.data(), 480, transportOutput);

    report("native float 48 kHz offline block processes", processed);
    report("native float entry preserves sub-int16 input values exactly",
           processor.normalizedFloat48Stereo().size()
                   == static_cast<int>(input.size() * sizeof(float))
               && std::memcmp(processor.normalizedFloat48Stereo().constData(),
                              input.data(), input.size() * sizeof(float)) == 0);
}

void testChannelStripRunsAt48k()
{
    ClientTube tube;
    tube.setEnabled(true);
    tube.setDriveDb(12.0f);
    tube.setDryWet(1.0f);

    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors processors;
    processors.tube = &tube;
    processor.setProcessors(processors);
    processor.setStageOrder(packedSingleStage(TxVoiceProcessor::Stage::Tube));
    processor.setMeasurementCaptureEnabled(true);
    processor.prepare(48000, 1024);
    QByteArray transportOutput = makeCanonicalTone(480, 48000, 1000.0f);
    const bool processed = processor.processCapturedInt16(transportOutput);

    report("channel-strip block processes", processed);
    report("tube is prepared at the canonical 48 kHz rate",
           std::fabs(tube.sampleRate() - 48000.0) < 0.1);
    report("enabled tube changes the post-strip measurement",
           processor.normalizedFloat48Stereo()
               != processor.postChannelStripFloat48Stereo());
}

void testNonFiniteSamplesCannotPoisonEgressSrc()
{
    std::vector<float> input(480 * 2, 0.1f);
    input[20] = std::nanf("");
    input[51] = std::numeric_limits<float>::infinity();
    input[92] = -std::numeric_limits<float>::infinity();

    TxVoiceProcessor processor;
    processor.prepare(48000, 480);
    QByteArray transportOutput;
    const bool processed = processor.processFloat48(
        input.data(), 480, transportOutput);

    report("non-finite input block still processes", processed);
    report("non-finite samples cannot poison float transport output",
           finiteFloatBuffer(processor.transportFloat32Stereo()));
}

void testMeasurementCaptureCanBeDisabled()
{
    TxVoiceProcessor processor;
    processor.setMeasurementCaptureEnabled(false);
    processor.prepare(48000, 1024);
    QByteArray transportOutput = makeCanonicalTone(480, 48000, 1000.0f);
    processor.processCapturedInt16(transportOutput);

    report("disabled normalized tap holds no copied block",
           processor.normalizedFloat48Stereo().isEmpty());
    report("disabled post-strip tap holds no copied block",
           processor.postChannelStripFloat48Stereo().isEmpty());
    report("transport output remains available with taps disabled",
           !transportOutput.isEmpty());
}

void testBlockBoundaryContinuityAndReset()
{
    const QByteArray input = makeCanonicalTone(4800, 48000, 997.0f);

    TxVoiceProcessor whole;
    whole.prepare(48000, 4800);
    QByteArray wholeOutput = input;
    whole.processCapturedInt16(wholeOutput);

    TxVoiceProcessor blocked;
    blocked.prepare(48000, 480);
    QByteArray blockedOutput;
    constexpr int kInputBlockBytes = 480 * 2 * static_cast<int>(sizeof(int16_t));
    for (int offset = 0; offset < input.size(); offset += kInputBlockBytes) {
        QByteArray block = input.mid(offset, kInputBlockBytes);
        const bool processed = blocked.processCapturedInt16(block);
        if (!processed) {
            report("streaming blocks all produce output", false);
            return;
        }
        blockedOutput.append(block);
    }

    report("48 -> 24 SRC is invariant to 10 ms block boundaries",
           blockedOutput == wholeOutput,
           "wholeBytes=" + std::to_string(wholeOutput.size())
               + " blockedBytes=" + std::to_string(blockedOutput.size()));

    blocked.reset();
    QByteArray afterReset;
    for (int offset = 0; offset < input.size(); offset += kInputBlockBytes) {
        QByteArray block = input.mid(offset, kInputBlockBytes);
        blocked.processCapturedInt16(block);
        afterReset.append(block);
    }
    report("reset restores deterministic SRC stream state",
           afterReset == blockedOutput);
}

void testOversizedCaptureBlockIsProcessed()
{
    constexpr int kPreparedInputFrames = 1024;
    constexpr int kInputFrames = 2000;
    constexpr int kFrameBytes = 2 * static_cast<int>(sizeof(int16_t));
    const QByteArray input = makeCanonicalTone(kInputFrames, 48000, 997.0f);

    TxVoiceProcessor oversized;
    oversized.prepare(48000, kPreparedInputFrames);
    QByteArray oversizedOutput = input;
    const bool oversizedProcessed = oversized.processCapturedInt16(
        oversizedOutput);

    TxVoiceProcessor partitioned;
    partitioned.prepare(48000, kPreparedInputFrames);
    QByteArray partitionedOutput;
    QByteArray firstBlock = input.left(kPreparedInputFrames * kFrameBytes);
    const bool firstProcessed = partitioned.processCapturedInt16(firstBlock);
    partitionedOutput.append(firstBlock);
    QByteArray secondBlock = input.mid(kPreparedInputFrames * kFrameBytes);
    const bool secondProcessed = partitioned.processCapturedInt16(secondBlock);
    partitionedOutput.append(secondBlock);

    report("capture block larger than prepared size is processed",
           oversizedProcessed);
    report("oversized capture block preserves expected transport frame count",
           oversizedOutput.size()
               == (kInputFrames / 2) * kFrameBytes,
           "bytes=" + std::to_string(oversizedOutput.size()));
    report("oversized capture processing is stream-continuous",
           firstProcessed && secondProcessed
               && oversizedOutput == partitionedOutput,
           "oversizedBytes=" + std::to_string(oversizedOutput.size())
               + " partitionedBytes="
               + std::to_string(partitionedOutput.size()));
}

void testTransportTpdfDither()
{
    constexpr int kInputFrames = 48000;
    std::vector<float> silence(kInputFrames * 2, 0.0f);
    std::vector<float> subLsbProbe(kInputFrames * 2, 1.0e-9f);

    TxVoiceProcessor processor;
    processor.prepare(48000, kInputFrames);
    QByteArray silentOutput;
    const bool processed = processor.processFloat48(
        silence.data(), kInputFrames, silentOutput);
    const QByteArray floatOutput = processor.transportFloat32Stereo();
    const auto* quantized = reinterpret_cast<const int16_t*>(
        silentOutput.constData());
    const int sampleCount = silentOutput.size()
        / static_cast<int>(sizeof(int16_t));

    bool exactSilence = true;
    for (int sample = 0; sample < sampleCount; ++sample) {
        exactSilence = quantized[sample] == 0 && exactSilence;
    }

    report("silent float transport block processes", processed);
    report("dither does not alter the float transport measurement tap",
           finiteFloatBuffer(floatOutput)
               && std::all_of(
                   reinterpret_cast<const float*>(floatOutput.constData()),
                   reinterpret_cast<const float*>(floatOutput.constData())
                       + floatOutput.size() / static_cast<int>(sizeof(float)),
                   [](float sample) { return sample == 0.0f; }));
    report("exact digital silence remains the zero int16 code", exactSilence);
    report("digital silence remains duplicated stereo",
           duplicatedStereo(silentOutput, false));

    QByteArray advancedProbeOutput;
    const bool probeProcessed = processor.processFloat48(
        subLsbProbe.data(), kInputFrames, advancedProbeOutput);
    const auto* probeSamples = reinterpret_cast<const int16_t*>(
        advancedProbeOutput.constData());
    const int probeSampleCount = advancedProbeOutput.size()
        / static_cast<int>(sizeof(int16_t));
    int nonZeroProbeSamples = 0;
    int64_t probeSampleSum = 0;
    for (int sample = 0; sample < probeSampleCount; ++sample) {
        nonZeroProbeSamples += probeSamples[sample] != 0 ? 1 : 0;
        probeSampleSum += probeSamples[sample];
    }
    report("nonzero sub-LSB signal is still TPDF dithered",
           probeProcessed && nonZeroProbeSamples > probeSampleCount / 10,
           "nonZero=" + std::to_string(nonZeroProbeSamples)
               + " samples=" + std::to_string(probeSampleCount));
    report("sub-LSB TPDF remains linked across duplicated channels",
           duplicatedStereo(advancedProbeOutput, false));
    report("sub-LSB TPDF retains near-zero DC bias",
           std::abs(probeSampleSum) < probeSampleCount / 100,
           "sum=" + std::to_string(probeSampleSum));

    TxVoiceProcessor freshProcessor;
    freshProcessor.prepare(48000, kInputFrames);
    QByteArray freshProbeOutput;
    freshProcessor.processFloat48(
        subLsbProbe.data(), kInputFrames, freshProbeOutput);
    report("silent frames continue advancing the dither PRNG",
           advancedProbeOutput != freshProbeOutput);

    processor.reset();
    QByteArray resetSilentOutput;
    processor.processFloat48(silence.data(), kInputFrames, resetSilentOutput);
    QByteArray afterReset;
    processor.processFloat48(subLsbProbe.data(), kInputFrames, afterReset);
    report("reset restores deterministic TPDF stream state",
           resetSilentOutput == silentOutput
               && afterReset == advancedProbeOutput);
}

void testDitheredTransportSaturatesAtInt16Rails()
{
    constexpr int kInputFrames = 48000;
    const auto verifyRail = [](float inputSample, int16_t expectedRail) {
        std::vector<float> input(kInputFrames * 2, inputSample);
        TxVoiceProcessor processor;
        processor.prepare(48000, kInputFrames);
        QByteArray int16Bytes;
        if (!processor.processFloat48(
            input.data(), kInputFrames, int16Bytes)) {
            return false;
        }

        const QByteArray& floatBytes = processor.transportFloat32Stereo();
        const auto* floatSamples = reinterpret_cast<const float*>(
            floatBytes.constData());
        const auto* int16Samples = reinterpret_cast<const int16_t*>(
            int16Bytes.constData());
        const int sampleCount = int16Bytes.size()
            / static_cast<int>(sizeof(int16_t));
        bool testedOverRangeSample = false;
        for (int sample = 0; sample < sampleCount; ++sample) {
            const bool overRange = expectedRail > 0
                ? floatSamples[sample] >= 1.0f
                : floatSamples[sample] <= -1.0f;
            if (overRange) {
                testedOverRangeSample = true;
                if (int16Samples[sample] != expectedRail) {
                    return false;
                }
            }
        }
        return testedOverRangeSample;
    };

    report("dithered quantizer saturates positive over-range samples",
           verifyRail(100.0f, std::numeric_limits<int16_t>::max()));
    report("dithered quantizer saturates negative over-range samples",
           verifyRail(-100.0f, std::numeric_limits<int16_t>::min()));
}

void testRnnoiseNative48kIsland()
{
    RNNoiseFilter rnnoise(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);
    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors processors;
    processor.setProcessors(processors);
    processor.setRnnoise(&rnnoise);
    processor.setRnnoiseEnabled(true);
    processor.prepare(48000, 480);

    bool allProcessed = true;
    bool allSized = true;
    bool allDuplicated = true;
    for (int block = 0; block < 12; ++block) {
        QByteArray transportOutput = makeCanonicalTone(
            480, 48000, 700.0f);
        allProcessed = processor.processCapturedInt16(transportOutput)
            && allProcessed;
        allSized = transportOutput.size()
            == 240 * 2 * static_cast<int>(sizeof(int16_t)) && allSized;
        allDuplicated = duplicatedStereo(
            transportOutput, false) && allDuplicated;
    }

    report("RNNoise native 48 kHz island processes complete frames", allProcessed);
    report("RNNoise path preserves 10 ms transport framing", allSized);
    report("TX RNNoise ProcessedMono remains duplicated stereo", allDuplicated);
}

void testDisabledRnnoiseIsNotDereferencedDuringPrepare()
{
    auto rnnoise = std::make_unique<RNNoiseFilter>(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);
    TxVoiceProcessor processor;
    processor.setRnnoise(rnnoise.get());
    processor.setRnnoiseEnabled(false);

    // Simulate the stale association that exposed the original ownership bug.
    // A disabled processor must not touch it while prepare() resets the graph.
    rnnoise.reset();
    const bool prepared = processor.prepare(48000, 480);
    processor.setRnnoise(nullptr);
    QByteArray transportOutput = makeCanonicalTone(480, 48000, 700.0f);
    const bool processed = processor.processCapturedInt16(transportOutput);

    report("disabled stale RNNoise association is not reset during prepare",
           prepared && processed);
}

void testLatencyAccounting()
{
    ClientGate gate;
    gate.setEnabled(true);
    gate.setLookaheadMs(2.0f);
    RNNoiseFilter rnnoise(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);

    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors processors;
    processors.gate = &gate;
    processor.setProcessors(processors);
    processor.setRnnoise(&rnnoise);
    processor.setStageOrder(packedSingleStage(TxVoiceProcessor::Stage::Gate));
    processor.setRnnoiseEnabled(true);
    processor.prepare(48000, 480);

    report("latency report includes RNNoise frame plus gate lookahead",
           processor.latencyFrames() == 394 + 480 + 96,
           "frames=" + std::to_string(processor.latencyFrames()));
}

// setRnnoise() is the single publication point for the one processor whose
// owner can retire it mid-stream. Pin the whole cycle: a published filter is
// used, unpublishing takes effect on the very next block without disturbing
// framing, and republishing a retained filter resumes cleanly. AudioEngine
// now keeps the RNNoiseFilter alive for its own lifetime and only unpublishes
// on disable, so this models the real ownership sequence.
void testRnnoisePublicationCycle()
{
    RNNoiseFilter rnnoise(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);
    TxVoiceProcessor processor;
    processor.setRnnoise(&rnnoise);
    processor.setRnnoiseEnabled(true);
    const bool prepared = processor.prepare(48000, 480);

    const auto runBlock = [&processor]() {
        QByteArray block = makeCanonicalTone(480, 48000, 700.0f);
        const bool ok = processor.processCapturedInt16(block);
        return std::make_pair(ok, block);
    };

    bool publishedOk = true;
    for (int block = 0; block < 4; ++block) {
        publishedOk = runBlock().first && publishedOk;
    }

    // Unpublish exactly the way AudioEngine's disable path does. The filter
    // object stays alive here, matching the retain-for-lifetime ownership.
    processor.setRnnoise(nullptr);
    const auto unpublished = runBlock();

    processor.setRnnoise(&rnnoise);
    rnnoise.reset();
    const auto republished = runBlock();

    const int expectedBytes = 240 * 2 * static_cast<int>(sizeof(int16_t));
    report("published RNNoise association processes blocks", prepared && publishedOk);
    report("unpublished RNNoise preserves 10 ms transport framing",
           unpublished.first && unpublished.second.size() == expectedBytes
               && duplicatedStereo(unpublished.second, false),
           "bytes=" + std::to_string(unpublished.second.size()));
    report("republishing a retained RNNoise filter resumes cleanly",
           republished.first && republished.second.size() == expectedBytes
               && duplicatedStereo(republished.second, false),
           "bytes=" + std::to_string(republished.second.size()));

    // Framing alone would still pass if setRnnoise(nullptr) were ignored, so
    // prove the stage is actually out of the chain: a processor unpublished
    // before its first block must be sample-identical to one that never had a
    // filter at all. RN2's 480-frame delay line makes any leak visible.
    TxVoiceProcessor unpublishedEarly;
    unpublishedEarly.setRnnoise(&rnnoise);
    unpublishedEarly.setRnnoiseEnabled(true);
    unpublishedEarly.prepare(48000, 480);
    unpublishedEarly.setRnnoise(nullptr);

    TxVoiceProcessor neverPublished;
    neverPublished.setRnnoiseEnabled(true);
    neverPublished.prepare(48000, 480);

    bool bypassMatches = true;
    for (int block = 0; block < 6; ++block) {
        QByteArray a = makeCanonicalTone(480, 48000, 700.0f);
        QByteArray b = makeCanonicalTone(480, 48000, 700.0f);
        const bool okA = unpublishedEarly.processCapturedInt16(a);
        const bool okB = neverPublished.processCapturedInt16(b);
        bypassMatches = okA && okB && a == b && bypassMatches;
    }
    report("unpublishing removes RNNoise from the chain, not just the guard",
           bypassMatches);

    // latencyFrames() must follow the same published state, since it is the
    // accessor callers use to reason about the chain.
    processor.setRnnoise(nullptr);
    const int withoutRnnoise = processor.latencyFrames();
    processor.setRnnoise(&rnnoise);
    const int withRnnoise = processor.latencyFrames();
    report("latencyFrames() tracks the published RNNoise association",
           withRnnoise == withoutRnnoise + 480,
           "without=" + std::to_string(withoutRnnoise)
               + " with=" + std::to_string(withRnnoise));
}

} // namespace

int main()
{
    testFixedRateContract();
    testVoiceSrcBandwidthAndAliasRejection();
    testCapturedRateSrcBandwidthAndAliasRejection();
    testVoiceSrcLatencyBudgets();
    testReusableBuffersPreserveCapacity();
    testCapturedTransportUsesCallerOwnedStorage();
    testResamplerResetRestoresFreshState();
    testStereoEgressMismatchSalvagesAndRealigns();
    test48kBypassAndMeasurementBoundaries();
    testDeviceRateNormalization();
    testFloat48OfflineEntryAvoidsInputQuantization();
    testChannelStripRunsAt48k();
    testNonFiniteSamplesCannotPoisonEgressSrc();
    testMeasurementCaptureCanBeDisabled();
    testBlockBoundaryContinuityAndReset();
    testOversizedCaptureBlockIsProcessed();
    testTransportTpdfDither();
    testDitheredTransportSaturatesAtInt16Rails();
    testRnnoiseNative48kIsland();
    testDisabledRnnoiseIsNotDereferencedDuringPrepare();
    testRnnoisePublicationCycle();
    testLatencyAccounting();

    std::printf("\n%s (%d failure%s)\n",
                g_failed == 0 ? "PASS" : "FAIL",
                g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
