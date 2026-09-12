#include "core/AudioEngine.h"
#include "core/SpectralNR.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using AetherSDR::AudioEngine;
using AetherSDR::SpectralNR;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", message);
    if (!condition) {
        ++g_failures;
    }
}

double rmsGainDb(const std::vector<float>& input,
                 const std::vector<float>& output,
                 int firstSample,
                 int lastSample,
                 int latencySamples)
{
    double inputEnergy = 0.0;
    double outputEnergy = 0.0;
    for (int i = firstSample; i < lastSample; ++i) {
        inputEnergy += static_cast<double>(input[i]) * input[i];
        outputEnergy += static_cast<double>(output[i + latencySamples])
                      * output[i + latencySamples];
    }
    return 10.0 * std::log10(
        std::max(outputEnergy, 1e-20) / std::max(inputEnergy, 1e-20));
}

struct Run {
    double settledDb{0.0};
    std::vector<float> input;
    std::vector<float> output;
};

void testTransientResetRetainsNoiseProfile()
{
    constexpr int kSampleRate = 24000;
    constexpr int kFftSize = 1024;
    constexpr int kOverlap = 4;
    constexpr int kBlockSamples = 73;
    constexpr int kSettleSamples = 6 * kSampleRate;
    constexpr int kResumeSamples = 6 * kSampleRate;

    // One continuous synthetic noise timeline. The reset point models the TX
    // gap, during which the bypassed filter receives no samples.
    std::vector<float> settle(kSettleSamples);
    std::vector<float> resume(kResumeSamples);
    std::uint32_t randomState = 0x33383231u;
    const auto nextWhite = [&randomState]() {
        randomState = 1664525u * randomState + 1013904223u;
        return 2.0 * (static_cast<double>(randomState) / 4294967295.0) - 1.0;
    };
    for (float& sample : settle) {
        sample = static_cast<float>(0.25 * nextWhite());
    }
    for (float& sample : resume) {
        sample = static_cast<float>(0.25 * nextWhite());
    }

    const auto runResumed = [&](bool transientReset, double resumeScale) {
        SpectralNR nr(kFftSize, kSampleRate, kOverlap);
        std::vector<float> settledOutput(kSettleSamples);
        int offset = 0;
        while (offset < kSettleSamples) {
            const int count = std::min(kBlockSamples, kSettleSamples - offset);
            nr.process(settle.data() + offset, settledOutput.data() + offset,
                       count);
            offset += count;
        }

        Run run;
        run.settledDb = rmsGainDb(settle, settledOutput, 4 * kSampleRate,
                                  5 * kSampleRate, kFftSize);
        if (transientReset) {
            nr.resetTransient();
        } else {
            nr.reset();
        }
        run.input.resize(kResumeSamples);
        for (int i = 0; i < kResumeSamples; ++i) {
            run.input[i] = static_cast<float>(resume[i] * resumeScale);
        }
        run.output.assign(kResumeSamples, 0.0f);
        offset = 0;
        while (offset < kResumeSamples) {
            const int count = std::min(kBlockSamples, kResumeSamples - offset);
            nr.process(run.input.data() + offset, run.output.data() + offset,
                       count);
            offset += count;
        }
        return run;
    };

    const auto postRampDb = [&](const Run& run) {
        return rmsGainDb(run.input, run.output, 11 * kSampleRate / 10,
                         8 * kSampleRate / 5, kFftSize);
    };
    const auto lateDb = [&](const Run& run) {
        return rmsGainDb(run.input, run.output, 5 * kSampleRate / 2,
                         3 * kSampleRate, kFftSize);
    };

    const Run transient = runResumed(true, 1.0);
    const Run full = runResumed(false, 1.0);
    const double settledDb = transient.settledDb;

    double stalePeak = 0.0;
    bool allFinite = true;
    for (int i = 0; i < kFftSize; ++i) {
        stalePeak = std::max(
            stalePeak, static_cast<double>(std::abs(transient.output[i])));
    }
    for (const float sample : transient.output) {
        allFinite = allFinite && std::isfinite(sample);
    }

    const double immediateDb = rmsGainDb(
        transient.input, transient.output, 0, 3 * kSampleRate / 20, kFftSize);
    const double transientPostRampDb = postRampDb(transient);
    const double fullPostRampDb = postRampDb(full);
    std::printf("settled %+.2f dB; immediate %+.2f dB; post-ramp warm "
                "%+.2f dB vs full %+.2f dB; late warm %+.2f dB vs full "
                "%+.2f dB; stale peak %.3g\n",
                settledDb, immediateDb, transientPostRampDb, fullPostRampDb,
                lateDb(transient), lateDb(full), stalePeak);

    check(std::abs(full.settledDb - transient.settledDb) < 0.01,
          "settle phase is identical across runs");
    check(allFinite, "warm-reset output stays finite");
    check(stalePeak < 1e-9, "warm reset flushes stale overlap-add audio");
    check(immediateDb > -3.0, "audio returns immediately on the dry signal");
    check(std::abs(transientPostRampDb - settledDb) < 2.5,
          "settled suppression depth returns when the ramp completes");
    check(std::abs(lateDb(transient) - settledDb) < 2.5
              && std::abs(lateDb(full) - settledDb) < 2.5,
          "warm and full resets converge at the late window");

    // A receiver AGC level step across the over cannot be observed by the
    // common-mode corrector. The retained estimate must still be no worse than
    // a full reset after the ramp and must settle within one OSMS window.
    for (const double scale : {2.0, 0.5}) {
        const Run stepTransient = runResumed(true, scale);
        const Run stepFull = runResumed(false, scale);
        const double stepPostRampTransient = postRampDb(stepTransient);
        const double stepPostRampFull = postRampDb(stepFull);
        const double stepLateTransient = lateDb(stepTransient);
        const double stepLateFull = lateDb(stepFull);
        std::printf("%+.0f dB step: post-ramp warm %+.2f dB vs full %+.2f "
                    "dB; late warm %+.2f dB vs full %+.2f dB\n",
                    20.0 * std::log10(scale), stepPostRampTransient,
                    stepPostRampFull, stepLateTransient, stepLateFull);
        check(stepPostRampTransient <= stepPostRampFull + 1.0,
              "level step is no worse than a full reset after the ramp");
        check(std::abs(stepLateTransient - stepTransient.settledDb) < 2.5
                  && std::abs(stepLateFull - stepFull.settledDb) < 2.5,
              "level step settles within one minimum-statistics window");
    }
}

// A weak wanted signal arriving just after unkey is the symptom #3821 is
// actually about ("weak callbacks masked by band noise"). The rows above
// measure suppression on noise alone, where deeper is always better — they
// cannot tell a better noise estimate apart from over-suppression that eats
// the other station. A retained floor is wrong in both directions after a
// receiver-AGC step: too low after an upward step, too high after a downward
// one, and the too-high case is the one that could chop speech.
//
// So measure what matters: the wanted signal's SNR gain, not its raw level.
// A weak carrier switches on 1.0 s after the edge (non-stationary, so the
// minimum-statistics tracker cannot have learned it as noise) and its
// amplitude is recovered by quadrature correlation over the same post-ramp
// window. The warm reset must not deliver less SNR improvement than the full
// reset it replaced, in any AGC direction.
void testWarmResetKeepsWeakSignalSnr()
{
    constexpr int kSampleRate = 24000;
    constexpr int kFftSize = 1024;
    constexpr int kOverlap = 4;
    constexpr int kBlockSamples = 73;
    constexpr int kSettleSamples = 6 * kSampleRate;
    constexpr int kResumeSamples = 6 * kSampleRate;
    constexpr double kToneHz = 1000.0;
    const int burstStart = kSampleRate;             // 1.0 s after the edge
    const int burstEnd = 17 * kSampleRate / 10;     // 1.7 s

    std::vector<float> settle(kSettleSamples);
    std::vector<float> resumeNoise(kResumeSamples);
    std::uint32_t randomState = 0x33383231u;
    const auto nextWhite = [&randomState]() {
        randomState = 1664525u * randomState + 1013904223u;
        return 2.0 * (static_cast<double>(randomState) / 4294967295.0) - 1.0;
    };
    for (float& sample : settle) {
        sample = static_cast<float>(0.25 * nextWhite());
    }
    for (float& sample : resumeNoise) {
        sample = static_cast<float>(0.25 * nextWhite());
    }

    const auto toneAmplitude = [&](const std::vector<float>& buffer,
                                   int firstSample, int lastSample,
                                   int latencySamples) {
        double real = 0.0;
        double imag = 0.0;
        for (int i = firstSample; i < lastSample; ++i) {
            const double phase = 2.0 * M_PI * kToneHz * i / kSampleRate;
            real += buffer[i + latencySamples] * std::cos(phase);
            imag += buffer[i + latencySamples] * std::sin(phase);
        }
        const double count = lastSample - firstSample;
        return 2.0 * std::sqrt(real * real + imag * imag) / count;
    };

    const auto runResumed = [&](bool transientReset, double resumeScale) {
        SpectralNR nr(kFftSize, kSampleRate, kOverlap);
        std::vector<float> settledOutput(kSettleSamples);
        for (int offset = 0; offset < kSettleSamples; offset += kBlockSamples) {
            nr.process(settle.data() + offset, settledOutput.data() + offset,
                       std::min(kBlockSamples, kSettleSamples - offset));
        }
        if (transientReset) {
            nr.resetTransient();
        } else {
            nr.reset();
        }
        // The wanted signal rides the receiver AGC exactly as the noise does,
        // so its input SNR is identical across the three scale rows.
        const double toneAmp = 0.02 * resumeScale;
        Run run;
        run.input.resize(kResumeSamples);
        for (int i = 0; i < kResumeSamples; ++i) {
            double value = resumeNoise[i] * resumeScale;
            if (i >= burstStart && i < burstEnd) {
                value += toneAmp
                    * std::sin(2.0 * M_PI * kToneHz * i / kSampleRate);
            }
            run.input[i] = static_cast<float>(value);
        }
        run.output.assign(kResumeSamples, 0.0f);
        for (int offset = 0; offset < kResumeSamples; offset += kBlockSamples) {
            nr.process(run.input.data() + offset, run.output.data() + offset,
                       std::min(kBlockSamples, kResumeSamples - offset));
        }
        return run;
    };

    const int firstSample = 11 * kSampleRate / 10;  // 1.1 s
    const int lastSample = 8 * kSampleRate / 5;     // 1.6 s
    for (const double scale : {1.0, 0.5, 2.0}) {
        const Run warm = runResumed(true, scale);
        const Run full = runResumed(false, scale);

        // Broadband gain over the window, and the wanted carrier's gain over
        // the same window. Their difference is the SNR the operator gets.
        const double warmBroadbandDb = rmsGainDb(warm.input, warm.output,
                                                 firstSample, lastSample,
                                                 kFftSize);
        const double fullBroadbandDb = rmsGainDb(full.input, full.output,
                                                 firstSample, lastSample,
                                                 kFftSize);
        const double inputTone =
            toneAmplitude(warm.input, firstSample, lastSample, 0);
        const double warmToneDb = 20.0 * std::log10(
            std::max(toneAmplitude(warm.output, firstSample, lastSample,
                                   kFftSize), 1e-12) / inputTone);
        const double fullToneDb = 20.0 * std::log10(
            std::max(toneAmplitude(full.output, firstSample, lastSample,
                                   kFftSize), 1e-12) / inputTone);
        const double warmSnrDb = warmToneDb - warmBroadbandDb;
        const double fullSnrDb = fullToneDb - fullBroadbandDb;

        std::printf("%+.0f dB step, weak signal: warm SNR %+.2f dB vs full "
                    "%+.2f dB (tone warm %+.2f dB, full %+.2f dB)\n",
                    20.0 * std::log10(scale), warmSnrDb, fullSnrDb,
                    warmToneDb, fullToneDb);
        // Two invariants, and the second one has a measured margin rather
        // than a round number. The warm path must still IMPROVE the weak
        // signal's SNR (measured +2.03 / +1.97 / +2.04 dB across the three
        // rows), and it must not fall meaningfully behind the full reset it
        // replaced. It leads at 0 dB (+2.03 vs +1.61) and +6 dB (+2.04 vs
        // +1.15), and trails by 0.44 dB at -6 dB — the downward-AGC-step
        // direction the resetTransient() comment calls out as the bounded
        // one, where the retained floor is temporarily too high. 1.0 dB
        // leaves that known gap room without letting a real regression pass.
        check(warmSnrDb > 1.0,
              "warm reset still improves weak-signal SNR after the edge");
        check(warmSnrDb >= fullSnrDb - 1.0,
              "warm reset stays within 1 dB of a full reset's weak-signal SNR");
    }
}

std::uint64_t diagnosticCount(const AudioEngine& engine, const char* key)
{
    const QJsonObject main = engine.nr2RuntimeDiagnostics()
                                 .value(QStringLiteral("main"))
                                 .toObject();
    return static_cast<std::uint64_t>(
        main.value(QLatin1String(key)).toDouble());
}

void testAudioEngineTxRxEdgeUsesWarmReset()
{
    AudioEngine engine;
    engine.setNr2Enabled(true);
    check(engine.nr2Enabled(), "AudioEngine creates the main NR2 instance");

    const std::uint64_t transientBefore =
        diagnosticCount(engine, "transientResetCount");
    const std::uint64_t noiseBefore =
        diagnosticCount(engine, "noiseEstimateResetCount");

    // The raw interlock, not TX ownership, drives the receive-chain bypass.
    // A foreign-client over therefore needs the same warm restart on unkey.
    engine.setRadioTransmitting(true, /*ownedByUs=*/false);
    engine.setRadioTransmitting(false, /*ownedByUs=*/false);

    const std::uint64_t transientAfter =
        diagnosticCount(engine, "transientResetCount");
    const std::uint64_t noiseAfter =
        diagnosticCount(engine, "noiseEstimateResetCount");
    check(transientAfter == transientBefore + 1,
          "AudioEngine TX->RX edge applies one transient reset");
    check(noiseAfter == noiseBefore,
          "AudioEngine TX->RX edge retains the noise estimate");

    engine.setRadioTransmitting(false, /*ownedByUs=*/false);
    check(diagnosticCount(engine, "transientResetCount") == transientAfter,
          "duplicate RX state does not reset NR2 again");
    engine.setNr2Enabled(false);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testTransientResetRetainsNoiseProfile();
    testWarmResetKeepsWeakSignalSnr();
    testAudioEngineTxRxEdgeUsesWarmReset();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
