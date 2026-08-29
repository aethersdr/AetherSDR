// Deterministic offline quality checks for the macOS minimum-statistics NR.
// CMake target: mac_nr_filter_test (macOS only).

#include "core/MacNRFilter.h"

#include <QByteArray>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {

constexpr int kRate = 24000;
constexpr int kBlockFrames = 240;
constexpr int kTotalFrames = 6 * kRate;
constexpr int kWarmupFrames = static_cast<int>(1.5 * kRate);
constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

int g_failed = 0;

void report(const char* name, bool ok, const char* detail)
{
    std::printf("%s %-64s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail);
    if (!ok) {
        ++g_failed;
    }
}

class NoiseSource {
public:
    explicit NoiseSource(uint32_t seed) : m_state(seed) {}

    float gaussian()
    {
        // Sum of twelve deterministic uniforms is an adequate N(0, 1)
        // approximation for this broadband attenuation measurement.
        float sum = 0.0f;
        for (int i = 0; i < 12; ++i) {
            m_state = m_state * 1664525u + 1013904223u;
            sum += static_cast<float>(m_state >> 8) / 16777216.0f;
        }
        return sum - 6.0f;
    }

private:
    uint32_t m_state;
};

std::vector<float> runFilter(const std::vector<float>& input, float strength)
{
    AetherSDR::MacNRFilter filter;
    if (!filter.isValid()) {
        return {};
    }
    filter.setStrength(strength);

    std::vector<float> output;
    output.reserve(input.size());
    for (int frame = 0; frame < static_cast<int>(input.size() / 2); frame += kBlockFrames) {
        const int count = std::min(kBlockFrames,
                                   static_cast<int>(input.size() / 2) - frame);
        QByteArray block(count * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
        std::copy_n(input.data() + 2 * frame, 2 * count,
                    reinterpret_cast<float*>(block.data()));
        const QByteArray processed = filter.process(block);
        if (processed.size() != block.size()) {
            return {};
        }
        const auto* values = reinterpret_cast<const float*>(processed.constData());
        output.insert(output.end(), values,
                      values + processed.size() / static_cast<int>(sizeof(float)));
    }
    return output;
}

double rms(const std::vector<float>& samples, int firstFrame, int lastFrame, int channel)
{
    double sum = 0.0;
    const int count = std::max(lastFrame - firstFrame, 0);
    for (int frame = firstFrame; frame < lastFrame; ++frame) {
        const double value = samples[2 * frame + channel];
        sum += value * value;
    }
    return count > 0 ? std::sqrt(sum / count) : 0.0;
}

double dbRatio(double numerator, double denominator)
{
    return 20.0 * std::log10(std::max(numerator, 1e-12) /
                             std::max(denominator, 1e-12));
}

std::vector<float> whiteNoise(float rmsValue, uint32_t seed)
{
    NoiseSource noise(seed);
    std::vector<float> samples(2 * kTotalFrames);
    for (int frame = 0; frame < kTotalFrames; ++frame) {
        const float value = rmsValue * noise.gaussian();
        samples[2 * frame] = value;
        samples[2 * frame + 1] = value;
    }
    return samples;
}

void test_white_noise_attenuation_and_scale_invariance()
{
    const std::vector<float> unitNoise = whiteNoise(1.0f, 0x53a7u);
    double referenceAttenuation = 0.0;
    bool scaleInvariant = true;
    char detail[192]{};

    for (const float inputRms : {0.025f, 0.05f, 0.10f}) {
        std::vector<float> scaled = unitNoise;
        for (float& sample : scaled) {
            sample *= inputRms;
        }
        const std::vector<float> output = runFilter(scaled, 1.0f);
        if (output.empty()) {
            report("MacNRFilter is available", false, "vDSP FFT setup failed");
            return;
        }
        const double inRms = rms(scaled, kWarmupFrames, kTotalFrames, 0);
        const double outRms = rms(output, kWarmupFrames, kTotalFrames, 0);
        const double attenuation = dbRatio(outRms, inRms);
        if (inputRms == 0.025f) {
            referenceAttenuation = attenuation;
        } else {
            scaleInvariant = scaleInvariant && std::abs(attenuation - referenceAttenuation) <= 1.0;
        }
        const bool withinNominalFloor = attenuation <= -12.0 && attenuation >= -26.0;
        std::snprintf(detail, sizeof(detail), "RMS=%.3f attenuation=%.2f dB", inputRms, attenuation);
        report("white-noise attenuation is 12 to 26 dB", withinNominalFloor,
               detail);
    }
    std::snprintf(detail, sizeof(detail), "reference attenuation=%.2f dB", referenceAttenuation);
    report("white-noise attenuation stays scale-invariant within 1 dB", scaleInvariant, detail);
}

void test_speech_like_snr_improvement()
{
    NoiseSource noise(0x1a2b3c4du);
    std::vector<float> noisy(2 * kTotalFrames);
    std::vector<float> desired(kTotalFrames);
    std::vector<float> background(kTotalFrames);
    for (int frame = 0; frame < kTotalFrames; ++frame) {
        const float t = static_cast<float>(frame) / kRate;
        // A varying voiced envelope with three formant-like components. The
        // envelope is deliberately nonstationary; MNR is not expected to
        // preserve a continuous unmodulated tone.
        const float envelope = 0.02f + 0.98f * std::pow(0.5f + 0.5f * std::sin(kTwoPi * 2.0f * t), 4.0f);
        desired[frame] = 0.15f * envelope * (
            0.72f * std::sin(kTwoPi * 430.0f * t)
          + 0.48f * std::sin(kTwoPi * 970.0f * t + 0.7f)
          + 0.32f * std::sin(kTwoPi * 1740.0f * t + 1.4f));
        background[frame] = 0.035f * noise.gaussian();

        // The shared MNR mask is synthesized independently into L/R.  Put
        // desired+noise on L and the identical noise on R so L-R isolates
        // the wanted component after the exact same adaptive mask.  Unlike
        // subtracting two independently adapted filter runs, this is a valid
        // decomposition for a nonlinear, stateful suppressor.
        noisy[2 * frame] = desired[frame] + background[frame];
        noisy[2 * frame + 1] = background[frame];
    }

    const std::vector<float> noisyOut = runFilter(noisy, 1.0f);
    if (noisyOut.empty()) {
        report("speech-like test filter construction", false, "vDSP FFT setup failed");
        return;
    }
    double desiredInputPower = 0.0;
    double noiseInputPower = 0.0;
    double desiredOutputPower = 0.0;
    double noiseOutputPower = 0.0;
    for (int frame = kWarmupFrames; frame < kTotalFrames; ++frame) {
        desiredInputPower += static_cast<double>(desired[frame]) * desired[frame];
        noiseInputPower += static_cast<double>(background[frame]) * background[frame];
        const double outputDesired = noisyOut[2 * frame] - noisyOut[2 * frame + 1];
        const double outputNoise = noisyOut[2 * frame + 1];
        desiredOutputPower += outputDesired * outputDesired;
        noiseOutputPower += outputNoise * outputNoise;
    }
    const double measuredFrames = kTotalFrames - kWarmupFrames;
    const double desiredIn = std::sqrt(desiredInputPower / measuredFrames);
    const double inputNoise = std::sqrt(noiseInputPower / measuredFrames);
    const double desiredOut = std::sqrt(desiredOutputPower / measuredFrames);
    const double outputNoise = std::sqrt(noiseOutputPower / measuredFrames);
    const double inputSnr = dbRatio(desiredIn, inputNoise);
    const double outputSnr = dbRatio(desiredOut, outputNoise);
    const double improvement = outputSnr - inputSnr;
    const double desiredLoss = dbRatio(desiredOut, desiredIn);
    char detail[192]{};
    std::snprintf(detail, sizeof(detail), "in=%.2f dB out=%.2f dB improvement=%.2f dB", inputSnr, outputSnr, improvement);
    report("speech-like multitone SNR improves by at least 6 dB", improvement >= 6.0, detail);
    std::snprintf(detail, sizeof(detail), "desired level change=%.2f dB", desiredLoss);
    report("speech-like desired-signal loss is at most 4 dB", desiredLoss >= -4.0, detail);
}

void test_strength_zero_reconstruction_and_stereo_balance()
{
    const std::vector<float> input = whiteNoise(0.08f, 0x9911u);
    const std::vector<float> bypass = runFilter(input, 0.0f);
    if (bypass.empty()) {
        report("strength-zero test filter construction", false, "vDSP FFT setup failed");
        return;
    }

    int bestLag = 0;
    double bestCorrelation = -1.0;
    for (int lag = 1; lag <= 1024; ++lag) {
        double dot = 0.0, inputPower = 0.0, outputPower = 0.0;
        for (int frame = kWarmupFrames + lag; frame < kTotalFrames; ++frame) {
            const double in = input[2 * (frame - lag)];
            const double out = bypass[2 * frame];
            dot += in * out;
            inputPower += in * in;
            outputPower += out * out;
        }
        const double correlation = dot / std::sqrt(inputPower * outputPower);
        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }
    const double bypassIn = rms(input, kWarmupFrames, kTotalFrames - bestLag, 0);
    const double bypassOut = rms(bypass, kWarmupFrames + bestLag, kTotalFrames, 0);
    char detail[192]{};
    std::snprintf(detail, sizeof(detail), "lag=%d frames correlation=%.6f level=%.3f dB", bestLag,
                  bestCorrelation, dbRatio(bypassOut, bypassIn));
    report("strength zero is a delayed, level-preserving reconstruction",
           bestLag > 0 && bestCorrelation >= 0.999 && std::abs(dbRatio(bypassOut, bypassIn)) <= 0.10,
           detail);

    NoiseSource noise(0x4411u);
    std::vector<float> stereo(2 * kTotalFrames);
    for (int frame = 0; frame < kTotalFrames; ++frame) {
        const float value = 0.04f * noise.gaussian();
        stereo[2 * frame] = 4.0f * value;
        stereo[2 * frame + 1] = value;
    }
    const std::vector<float> stereoOut = runFilter(stereo, 1.0f);
    if (stereoOut.empty()) {
        report("stereo balance test filter construction", false, "vDSP FFT setup failed");
        return;
    }
    const double ratio = rms(stereoOut, kWarmupFrames, kTotalFrames, 0)
                       / rms(stereoOut, kWarmupFrames, kTotalFrames, 1);
    std::snprintf(detail, sizeof(detail), "output L/R RMS ratio=%.4f", ratio);
    report("shared MNR mask preserves a 4:1 stereo ratio", std::abs(ratio - 4.0) <= 0.05, detail);
}

void test_zero_prefill_and_reset_do_not_poison_noise_history()
{
    AetherSDR::MacNRFilter filter;
    if (!filter.isValid()) {
        report("startup/reset test filter construction", false, "vDSP FFT setup failed");
        return;
    }
    filter.setStrength(1.0f);

    QByteArray silence(kBlockFrames * 2 * static_cast<int>(sizeof(float)),
                       Qt::Uninitialized);
    std::fill_n(reinterpret_cast<float*>(silence.data()),
                kBlockFrames * 2, 0.0f);
    const QByteArray initialOutput = filter.process(silence);
    bool initialOutputValid = initialOutput.size() == silence.size();
    const auto* initialSamples =
        reinterpret_cast<const float*>(initialOutput.constData());
    for (int i = 0; initialOutputValid && i < kBlockFrames * 2; ++i) {
        initialOutputValid = std::isfinite(initialSamples[i])
                          && initialSamples[i] == 0.0f;
    }
    report("latency-prefill silence stays finite and transparent",
           initialOutputValid, initialOutputValid ? "all samples are zero" : "invalid output");

    filter.reset();
    constexpr int kResetFrames = 2 * kRate;
    NoiseSource noise(0x7812u);
    std::vector<float> input(2 * kResetFrames);
    std::vector<float> output;
    output.reserve(input.size());
    for (int frame = 0; frame < kResetFrames; ++frame) {
        const float value = 0.05f * noise.gaussian();
        input[2 * frame] = value;
        input[2 * frame + 1] = value;
    }
    for (int frame = 0; frame < kResetFrames; frame += kBlockFrames) {
        QByteArray block(kBlockFrames * 2 * static_cast<int>(sizeof(float)),
                         Qt::Uninitialized);
        std::copy_n(input.data() + 2 * frame, kBlockFrames * 2,
                    reinterpret_cast<float*>(block.data()));
        const QByteArray processed = filter.process(block);
        const auto* values = reinterpret_cast<const float*>(processed.constData());
        output.insert(output.end(), values,
                      values + processed.size() / static_cast<int>(sizeof(float)));
    }
    const int measureFrom = kResetFrames - kRate / 2;
    const double attenuation = dbRatio(
        rms(output, measureFrom, kResetFrames, 0),
        rms(input, measureFrom, kResetFrames, 0));
    char detail[128]{};
    std::snprintf(detail, sizeof(detail), "post-reset attenuation=%.2f dB", attenuation);
    report("reset estimator relearns stationary noise", attenuation <= -12.0, detail);
}

void test_silence_gap_preserves_learned_noise_floor()
{
    constexpr int kTrainingFrames = 2 * kRate;
    constexpr int kSilenceFrames = kRate;
    constexpr int kResumeFrames = 2 * kRate;
    constexpr int kResumeStart = kTrainingFrames + kSilenceFrames;
    constexpr int kMeasureStart = kResumeStart + 512;
    constexpr int kMeasureEnd = kResumeStart + kRate / 5;

    NoiseSource noise(0x51a7u);
    std::vector<float> input(2 * (kTrainingFrames + kSilenceFrames + kResumeFrames), 0.0f);
    for (int frame = 0; frame < kTrainingFrames; ++frame) {
        const float value = 0.05f * noise.gaussian();
        input[2 * frame] = value;
        input[2 * frame + 1] = value;
    }
    for (int frame = kResumeStart;
         frame < kTrainingFrames + kSilenceFrames + kResumeFrames; ++frame) {
        const float value = 0.05f * noise.gaussian();
        input[2 * frame] = value;
        input[2 * frame + 1] = value;
    }

    const std::vector<float> output = runFilter(input, 1.0f);
    if (output.empty()) {
        report("silence-gap test filter construction", false, "vDSP FFT setup failed");
        return;
    }
    const double attenuation = dbRatio(
        rms(output, kMeasureStart, kMeasureEnd, 0),
        rms(input, kMeasureStart, kMeasureEnd, 0));
    char detail[128]{};
    std::snprintf(detail, sizeof(detail), "early post-silence attenuation=%.2f dB", attenuation);
    report("silence gap does not erase the learned noise floor",
           attenuation <= -8.0, detail);
}

void test_near_silence_does_not_seed_noise_floor()
{
    constexpr int kDitherFrames = kRate;
    constexpr int kNoiseFrames = 2 * kRate;
    constexpr int kMeasureStart = kDitherFrames + 512;
    constexpr int kMeasureEnd = kDitherFrames + kRate / 4;
    constexpr float kOnePcmLsb = 1.0f / 32768.0f;

    NoiseSource noise(0xd17eu);
    std::vector<float> input(2 * (kDitherFrames + kNoiseFrames));
    for (int frame = 0; frame < kDitherFrames; ++frame) {
        const float value = (frame % 2 == 0) ? kOnePcmLsb : -kOnePcmLsb;
        input[2 * frame] = value;
        input[2 * frame + 1] = value;
    }
    for (int frame = kDitherFrames; frame < kDitherFrames + kNoiseFrames; ++frame) {
        const float value = 0.05f * noise.gaussian();
        input[2 * frame] = value;
        input[2 * frame + 1] = value;
    }

    std::vector<float> silencePrefixed = input;
    std::fill(silencePrefixed.begin(),
              silencePrefixed.begin() + 2 * kDitherFrames, 0.0f);
    const std::vector<float> ditherOutput = runFilter(input, 1.0f);
    const std::vector<float> silenceOutput = runFilter(silencePrefixed, 1.0f);
    if (ditherOutput.empty() || silenceOutput.empty()) {
        report("near-silence seed test filter construction", false, "vDSP FFT setup failed");
        return;
    }
    const double ditherAttenuation = dbRatio(
        rms(ditherOutput, kMeasureStart, kMeasureEnd, 0),
        rms(input, kMeasureStart, kMeasureEnd, 0));
    const double silenceAttenuation = dbRatio(
        rms(silenceOutput, kMeasureStart, kMeasureEnd, 0),
        rms(silencePrefixed, kMeasureStart, kMeasureEnd, 0));
    char detail[192]{};
    std::snprintf(detail, sizeof(detail),
                  "dither=%.2f dB exact-silence=%.2f dB delta=%.2f dB",
                  ditherAttenuation, silenceAttenuation,
                  ditherAttenuation - silenceAttenuation);
    report("one-LSB dither does not delay noise-floor acquisition",
           std::abs(ditherAttenuation - silenceAttenuation) <= 1.0, detail);
}

void test_enable_during_speech_avoids_deep_ducking()
{
    constexpr int kFrames = 2 * kRate;
    constexpr int kLatencyFrames = 512;
    constexpr int kMeasureFrames = kRate / 10;
    NoiseSource noise(0x5ee1u);
    std::vector<float> input(2 * kFrames);
    std::vector<float> desired(kFrames);
    for (int frame = 0; frame < kFrames; ++frame) {
        const float t = static_cast<float>(frame) / kRate;
        const float envelope = 0.15f + 0.85f
            * std::pow(0.5f + 0.5f * std::sin(kTwoPi * 3.0f * t), 4.0f);
        desired[frame] = 0.16f * envelope * (
            0.70f * std::sin(kTwoPi * 430.0f * t)
          + 0.45f * std::sin(kTwoPi * 970.0f * t + 0.7f)
          + 0.30f * std::sin(kTwoPi * 1740.0f * t + 1.4f));
        const float background = 0.025f * noise.gaussian();
        input[2 * frame] = desired[frame] + background;
        input[2 * frame + 1] = background;
    }

    const std::vector<float> output = runFilter(input, 1.0f);
    if (output.empty()) {
        report("mid-speech enable test filter construction", false, "vDSP FFT setup failed");
        return;
    }
    double desiredInputPower = 0.0;
    double desiredOutputPower = 0.0;
    for (int offset = 0; offset < kMeasureFrames; ++offset) {
        const double inputDesired = desired[offset];
        const int outputFrame = kLatencyFrames + offset;
        const double outputDesired = output[2 * outputFrame]
                                   - output[2 * outputFrame + 1];
        desiredInputPower += inputDesired * inputDesired;
        desiredOutputPower += outputDesired * outputDesired;
    }
    const double desiredLoss = 10.0 * std::log10(
        std::max(desiredOutputPower, 1e-12) /
        std::max(desiredInputPower, 1e-12));
    char detail[128]{};
    std::snprintf(detail, sizeof(detail), "first 100 ms desired loss=%.2f dB", desiredLoss);
    report("enabling MNR during speech avoids deep startup ducking",
           desiredLoss >= -1.75, detail);
}

} // namespace

int main()
{
    test_white_noise_attenuation_and_scale_invariance();
    test_speech_like_snr_improvement();
    test_strength_zero_reconstruction_and_stereo_balance();
    test_zero_prefill_and_reset_do_not_poison_noise_history();
    test_silence_gap_preserves_learned_noise_floor();
    test_near_silence_does_not_seed_noise_floor();
    test_enable_during_speech_avoids_deep_ducking();
    std::printf(g_failed == 0 ? "\nPASSED\n" : "\nFAILED (%d)\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
