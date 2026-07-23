// Standalone regression and quality harness for SpectralNR.
// CMake target `spectral_nr_test`.  Exit 0 = pass.
//
// Regression for #1507: bessI0e / bessI1e must remain finite well beyond the
// runtime SNR cap, and the Gaussian-linear gain must not NaN-clamp to 0.01 for
// strong signals.

#include "core/SpectralNR.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

using AetherSDR::SpectralNR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const char* detail = nullptr)
{
    std::printf("%s %-70s%s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail ? detail : "");
    if (!ok) ++g_failed;
}

// ─── Reference unscaled Bessel I0 / I1 (A&S, safe only for small x) ──────────
// Used only for equivalence checks at x values that don't overflow.

double bessI0_ref(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        return 1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492
             + t * (0.2659732 + t * (0.0360768 + t * 0.0045813)))));
    }
    double t = 3.75 / ax;
    return (std::exp(ax) / std::sqrt(ax))
         * (0.39894228 + t * (0.01328592 + t * (0.00225319
          + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706
          + t * (0.02635537 + t * (-0.01647633 + t * 0.00392377))))))));
}

double bessI1_ref(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        double val = ax * (0.5 + t * (0.87890594 + t * (0.51498869
                   + t * (0.15084934 + t * (0.02658733 + t * (0.00301532
                   + t * 0.00032411))))));
        return x < 0.0 ? -val : val;
    }
    double t = 3.75 / ax;
    double val = (std::exp(ax) / std::sqrt(ax))
               * (0.39894228 + t * (-0.03988024 + t * (-0.00362018
                + t * (0.00163801 + t * (-0.01031555 + t * (0.02282967
                + t * (-0.02895312 + t * (0.01787654 - t * 0.00420059))))))));
    return x < 0.0 ? -val : val;
}

// ─── Local copies of the scaled Bessel functions ─────────────────────────────
// bessI0e / bessI1e are private statics in SpectralNR.  We keep the same
// A&S polynomial coefficients here so we can test the math independently,
// then rely on SpectralNR::process() for full integration coverage.

double bessI0e(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        return std::exp(-ax) * (1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492
             + t * (0.2659732 + t * (0.0360768 + t * 0.0045813))))));
    }
    double t = 3.75 / ax;
    return (1.0 / std::sqrt(ax))
         * (0.39894228 + t * (0.01328592 + t * (0.00225319
          + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706
          + t * (0.02635537 + t * (-0.01647633 + t * 0.00392377))))))));
}

double bessI1e(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        double val = std::exp(-ax) * ax * (0.5 + t * (0.87890594 + t * (0.51498869
                   + t * (0.15084934 + t * (0.02658733 + t * (0.00301532
                   + t * 0.00032411))))));
        return x < 0.0 ? -val : val;
    }
    double t = 3.75 / ax;
    double val = (1.0 / std::sqrt(ax))
               * (0.39894228 + t * (-0.03988024 + t * (-0.00362018
                + t * (0.00163801 + t * (-0.01031555 + t * (0.02282967
                + t * (-0.02895312 + t * (0.01787654 - t * 0.00420059))))))));
    return x < 0.0 ? -val : val;
}

// ─── Test groups ─────────────────────────────────────────────────────────────

void test_bessel_finiteness()
{
    // For each v in this set, bessI0e(v/2) and bessI1e(v/2) must be finite.
    // Stress v up to 1e4. Values above 1420 triggered the old overflow even
    // though the runtime estimator now applies a much lower physical SNR cap.
    const std::vector<double> v_values = {
        0.0, 0.1, 1.0, 10.0, 100.0, 500.0,
        1000.0, 1419.0, 1420.0, 1421.0,   // straddle old overflow threshold
        2000.0, 5000.0, 10000.0
    };

    for (double v : v_values) {
        double arg = 0.5 * v;
        double i0 = bessI0e(arg);
        double i1 = bessI1e(arg);

        char name[128];
        std::snprintf(name, sizeof(name), "bessI0e(%.1f) finite [v=%.0f]", arg, v);
        report(name, std::isfinite(i0));

        std::snprintf(name, sizeof(name), "bessI1e(%.1f) finite [v=%.0f]", arg, v);
        report(name, std::isfinite(i1));

        // Scaled Bessel must be non-negative (I0 is always >= 1, I1 >= 0 for x >= 0)
        std::snprintf(name, sizeof(name), "bessI0e(%.1f) >= 0 [v=%.0f]", arg, v);
        report(name, i0 >= 0.0);

        std::snprintf(name, sizeof(name), "bessI1e(%.1f) >= 0 for x>=0 [v=%.0f]", arg, v);
        report(name, arg == 0.0 || i1 >= 0.0);
    }
}

void test_bessel_equivalence()
{
    // At x values where the old unscaled code didn't overflow (<= 50),
    // bessI0e(x) must equal exp(-x) * bessI0_ref(x) to within 1e-9 relative.
    const std::vector<double> xs = {0.01, 0.1, 0.5, 1.0, 2.0, 3.0, 3.74, 3.75, 4.0, 5.0, 10.0, 30.0, 50.0};
    constexpr double kTol = 1e-9;

    for (double x : xs) {
        double scale = std::exp(-x);
        double ref0 = scale * bessI0_ref(x);
        double ref1 = scale * bessI1_ref(x);

        double got0 = bessI0e(x);
        double got1 = bessI1e(x);

        double err0 = std::abs(got0 - ref0) / std::max(std::abs(ref0), 1e-300);
        double err1 = std::abs(got1 - ref1) / std::max(std::abs(ref1), 1e-300);

        char name[128];
        std::snprintf(name, sizeof(name), "bessI0e(%.4f) matches exp(-x)*I0_ref (rel err=%.2e)", x, err0);
        report(name, err0 < kTol);

        std::snprintf(name, sizeof(name), "bessI1e(%.4f) matches exp(-x)*I1_ref (rel err=%.2e)", x, err1);
        report(name, err1 < kTol);
    }

    // bessI0e is symmetric (I0 is even): bessI0e(-x) == bessI0e(x)
    for (double x : {1.0, 3.75, 10.0, 50.0}) {
        double pos = bessI0e(x);
        double neg = bessI0e(-x);
        char name[128];
        std::snprintf(name, sizeof(name), "bessI0e symmetry at x=%.1f", x);
        report(name, std::abs(pos - neg) < 1e-15);
    }

    // bessI1e is antisymmetric: bessI1e(-x) == -bessI1e(x)
    for (double x : {1.0, 3.75, 10.0, 50.0}) {
        double pos = bessI1e(x);
        double neg = bessI1e(-x);
        char name[128];
        std::snprintf(name, sizeof(name), "bessI1e antisymmetry at x=%.1f", x);
        report(name, std::abs(pos + neg) < 1e-15);
    }
}

void test_gain_finiteness()
{
    // Feed 1000 hops of full-scale 1 kHz sine through SpectralNR's
    // Gaussian-linear method, which exercises the Bessel expression.
    // Every output sample must be finite — no NaN from Bessel overflow.
    // SpectralNR does not clamp output to ±1.0 (that is AudioEngine's job),
    // so only finiteness is asserted here.

    SpectralNR nr(1024, 24000, 4);
    nr.setGainMethod(0);   // WDSP Gaussian, linear-amplitude path

    const int hopSize = 256;
    const int sampleRate = 24000;
    const double freq = 1000.0;

    std::vector<float> inBuf(hopSize), outBuf(hopSize);
    int nanCount = 0;

    for (int hop = 0; hop < 1000; ++hop) {
        int offset = hop * hopSize;
        for (int i = 0; i < hopSize; ++i) {
            double t = static_cast<double>(offset + i) / sampleRate;
            inBuf[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * freq * t));
        }
        nr.process(inBuf.data(), outBuf.data(), hopSize);
        for (int i = 0; i < hopSize; ++i) {
            if (!std::isfinite(outBuf[i])) ++nanCount;
        }
    }

    char detail[64];
    std::snprintf(detail, sizeof(detail), " (NaN count: %d)", nanCount);
    report("gain_finiteness: no NaN/Inf in 1000 hops of 1 kHz full-scale sine", nanCount == 0, detail);
}

std::vector<float> buildSyntheticAudio(int samples)
{
    constexpr int sampleRate = 24000;
    std::vector<float> audio(samples);
    for (int i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        audio[i] = static_cast<float>(
            0.30 * std::sin(2.0 * std::numbers::pi * 720.0 * t)
          + 0.18 * std::sin(2.0 * std::numbers::pi * 1740.0 * t)
          + 0.04 * std::sin(2.0 * std::numbers::pi * 43.0 * t));
    }
    return audio;
}

std::vector<float> processWithBlockSize(const std::vector<float>& input,
                                        int blockSamples)
{
    SpectralNR nr;
    nr.setGainMethod(2);

    std::vector<float> output(input.size());
    int offset = 0;
    while (offset < static_cast<int>(input.size())) {
        const int count = std::min(blockSamples,
                                   static_cast<int>(input.size()) - offset);
        nr.process(input.data() + offset, output.data() + offset, count);
        offset += count;
    }
    return output;
}

std::vector<float> processWithGeometry(const std::vector<float>& input,
                                       int fftSize,
                                       int overlap,
                                       int blockSamples,
                                       float gainFloor = 0.00f,
                                       bool aeFilter = true,
                                       int gainMethod = 2,
                                       float gainSmooth = 0.85f,
                                       int npeMethod = 0,
                                       bool useLegacyGainMethods = false,
                                       float qSpp = 0.20f)
{
    SpectralNR nr(fftSize, 24000, overlap, useLegacyGainMethods);
    nr.setGainMethod(gainMethod);
    nr.setNpeMethod(npeMethod);
    nr.setGainFloor(gainFloor);
    nr.setAeFilter(aeFilter);
    nr.setGainSmooth(gainSmooth);
    nr.setQspp(qSpp);

    std::vector<float> output(input.size());
    int offset = 0;
    while (offset < static_cast<int>(input.size())) {
        const int count = std::min(blockSamples,
                                   static_cast<int>(input.size()) - offset);
        nr.process(input.data() + offset, output.data() + offset, count);
        offset += count;
    }
    return output;
}

struct GeometryMetrics {
    double noiseAttenuationDb{0.0};
    double onsetSpeechGainDb{0.0};
    double speechGainDb{0.0};
    double outputSnrDb{0.0};
    double residualFlatness{0.0};
};

double meanSpectralFlatness(const std::vector<float>& samples,
                            int firstSample,
                            int lastSample)
{
    constexpr int frameSize = 512;
    constexpr int frameStride = 1024;
    constexpr int firstBin = 2;
    constexpr int lastBin = 128;
    double flatnessSum = 0.0;
    int frameCount = 0;
    for (int frameStart = firstSample;
         frameStart + frameSize <= lastSample;
         frameStart += frameStride) {
        double logPowerSum = 0.0;
        double powerSum = 0.0;
        int binCount = 0;
        for (int k = firstBin; k <= lastBin; ++k) {
            double real = 0.0;
            double imag = 0.0;
            for (int n = 0; n < frameSize; ++n) {
                const double window = 0.5 * (1.0 - std::cos(
                    2.0 * std::numbers::pi * n / frameSize));
                const double phase = 2.0 * std::numbers::pi * k * n / frameSize;
                const double value = window * samples[frameStart + n];
                real += value * std::cos(phase);
                imag -= value * std::sin(phase);
            }
            const double power = std::max(real * real + imag * imag, 1e-20);
            logPowerSum += std::log(power);
            powerSum += power;
            ++binCount;
        }
        const double geometricMean = std::exp(logPowerSum / binCount);
        const double arithmeticMean = powerSum / binCount;
        flatnessSum += geometricMean / std::max(arithmeticMean, 1e-20);
        ++frameCount;
    }
    return flatnessSum / std::max(frameCount, 1);
}

GeometryMetrics measureGeometry(int fftSize,
                                int overlap,
                                float gainFloor = 0.00f,
                                bool aeFilter = true,
                                int gainMethod = 2,
                                float gainSmooth = 0.85f,
                                int npeMethod = 0,
                                bool useLegacyGainMethods = false,
                                float qSpp = 0.20f,
                                float speechScale = 1.0f)
{
    constexpr int sampleRate = 24000;
    constexpr int durationSeconds = 12;
    constexpr int speechStart = 4 * sampleRate;
    const int totalSamples = durationSeconds * sampleRate;
    std::vector<float> clean(totalSamples, 0.0f);
    std::vector<float> noise(totalSamples, 0.0f);
    std::vector<float> input(totalSamples, 0.0f);

    std::uint32_t randomState = 0x4e523232u;
    double phase = 0.0;
    for (int i = 0; i < totalSamples; ++i) {
        randomState = 1664525u * randomState + 1013904223u;
        const double white =
            2.0 * (static_cast<double>(randomState) / 4294967295.0) - 1.0;
        noise[i] = static_cast<float>(0.10 * white);

        if (i >= speechStart) {
            const double t = static_cast<double>(i - speechStart) / sampleRate;
            const double f0 = 135.0 + 18.0 * std::sin(2.0 * std::numbers::pi * 0.65 * t);
            phase += 2.0 * std::numbers::pi * f0 / sampleRate;
            const double syllable = 0.25 + 0.75 * std::pow(
                0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * 2.7 * t), 2.0);
            clean[i] = static_cast<float>(speechScale * syllable * (
                0.22 * std::sin(phase)
              + 0.12 * std::sin(2.0 * phase)
              + 0.07 * std::sin(3.0 * phase)));
        }
        input[i] = clean[i] + noise[i];
    }

    const std::vector<float> output =
        processWithGeometry(input, fftSize, overlap, 73, gainFloor,
                            aeFilter, gainMethod, gainSmooth, npeMethod,
                            useLegacyGainMethods, qSpp);

    double noiseInEnergy = 0.0;
    double noiseOutEnergy = 0.0;
    for (int i = 2 * sampleRate; i < speechStart - fftSize; ++i) {
        const double inSample = input[i];
        const double outSample = output[i + fftSize];
        noiseInEnergy += inSample * inSample;
        noiseOutEnergy += outSample * outSample;
    }

    double cleanEnergy = 0.0;
    double outputCleanDot = 0.0;
    for (int i = 6 * sampleRate; i < 10 * sampleRate; ++i) {
        const double cleanSample = clean[i];
        cleanEnergy += cleanSample * cleanSample;
        outputCleanDot += output[i + fftSize] * cleanSample;
    }
    const double speechGain = outputCleanDot / std::max(cleanEnergy, 1e-20);

    double onsetCleanEnergy = 0.0;
    double onsetOutputCleanDot = 0.0;
    for (int i = speechStart + fftSize;
         i < speechStart + sampleRate / 2;
         ++i) {
        const double cleanSample = clean[i];
        onsetCleanEnergy += cleanSample * cleanSample;
        onsetOutputCleanDot += output[i + fftSize] * cleanSample;
    }
    const double onsetSpeechGain = onsetOutputCleanDot
        / std::max(onsetCleanEnergy, 1e-20);

    double residualEnergy = 0.0;
    for (int i = 6 * sampleRate; i < 10 * sampleRate; ++i) {
        const double residual = output[i + fftSize] - speechGain * clean[i];
        residualEnergy += residual * residual;
    }

    GeometryMetrics metrics;
    metrics.noiseAttenuationDb = 10.0 * std::log10(
        std::max(noiseOutEnergy, 1e-20) / std::max(noiseInEnergy, 1e-20));
    metrics.onsetSpeechGainDb = 20.0 * std::log10(
        std::max(std::abs(onsetSpeechGain), 1e-20));
    metrics.speechGainDb = 20.0 * std::log10(std::max(std::abs(speechGain), 1e-20));
    metrics.outputSnrDb = 10.0 * std::log10(
        std::max(speechGain * speechGain * cleanEnergy, 1e-20)
        / std::max(residualEnergy, 1e-20));
    metrics.residualFlatness = meanSpectralFlatness(
        output, 2 * sampleRate + fftSize, speechStart);
    return metrics;
}

void test_default_parameter_profiles()
{
    struct Profile {
        const char* name;
        float gainFloor;
        float gainSmooth;
        int npeMethod;
        float qSpp;
    };
    const Profile profiles[] = {
        {"default",   0.00f, 0.85f, 0, 0.20f},
        {"former",    0.10f, 0.85f, 0, 0.20f},
        {"threshold", 0.00f, 0.85f, 0, 0.35f},
        {"floor",     0.05f, 0.85f, 0, 0.20f},
        {"balanced",  0.05f, 0.85f, 0, 0.35f},
        {"smoother",  0.05f, 0.90f, 0, 0.35f},
        {"MMSE",      0.05f, 0.85f, 1, 0.35f},
        {"maximum",   0.00f, 0.85f, 1, 0.35f},
    };

    GeometryMetrics current;
    GeometryMetrics former;
    for (const Profile& profile : profiles) {
        const GeometryMetrics metrics = measureGeometry(
            1024, 4, profile.gainFloor, true, 2, profile.gainSmooth,
            profile.npeMethod, false, profile.qSpp);
        if (profile.name == std::string_view("default")) {
            current = metrics;
        } else if (profile.name == std::string_view("former")) {
            former = metrics;
        }
        std::printf(" profile %-9s: noise %+.2f dB, onset %+.2f dB, "
                    "speech %+.2f dB, SNR %+.2f dB, flatness %.4f\n",
                    profile.name, metrics.noiseAttenuationDb,
                    metrics.onsetSpeechGainDb, metrics.speechGainDb,
                    metrics.outputSnrDb, metrics.residualFlatness);
    }

    report("default_profile: current settings preserve speech onset",
           current.onsetSpeechGainDb > -12.0);
    report("default_profile: zero Naturalness adds at least 4 dB noise suppression",
           current.noiseAttenuationDb < former.noiseAttenuationDb - 4.0);
    report("default_profile: zero Naturalness preserves speech onset within 1 dB",
           current.onsetSpeechGainDb > former.onsetSpeechGainDb - 1.0);

    const GeometryMetrics weakCurrent = measureGeometry(
        1024, 4, 0.00f, true, 2, 0.85f, 0, false, 0.20f, 0.25f);
    const GeometryMetrics weakFormer = measureGeometry(
        1024, 4, 0.10f, true, 2, 0.85f, 0, false, 0.20f, 0.25f);
    std::printf(" weak current : onset %+.2f dB, speech %+.2f dB, SNR %+.2f dB\n",
                weakCurrent.onsetSpeechGainDb, weakCurrent.speechGainDb,
                weakCurrent.outputSnrDb);
    std::printf(" weak former  : onset %+.2f dB, speech %+.2f dB, SNR %+.2f dB\n",
                weakFormer.onsetSpeechGainDb, weakFormer.speechGainDb,
                weakFormer.outputSnrDb);
    report("default_profile: zero Naturalness preserves weak-speech onset within 1 dB",
           weakCurrent.onsetSpeechGainDb > weakFormer.onsetSpeechGainDb - 1.0);
}

void test_geometry_quality_comparison()
{
    const GeometryMetrics current = measureGeometry(256, 2);
    const GeometryMetrics compact = measureGeometry(512, 4);
    const GeometryMetrics lowerLatency = measureGeometry(1024, 4);
    const GeometryMetrics equivalent = measureGeometry(2048, 4);
    const GeometryMetrics largest = measureGeometry(4096, 4);

    std::printf("       256/2: noise %+.2f dB, speech %+.2f dB, output SNR %+.2f dB\n",
                current.noiseAttenuationDb, current.speechGainDb, current.outputSnrDb);
    std::printf("       512/4: noise %+.2f dB, speech %+.2f dB, output SNR %+.2f dB\n",
                compact.noiseAttenuationDb, compact.speechGainDb,
                compact.outputSnrDb);
    std::printf("      1024/4: noise %+.2f dB, speech %+.2f dB, output SNR %+.2f dB\n",
                lowerLatency.noiseAttenuationDb, lowerLatency.speechGainDb,
                lowerLatency.outputSnrDb);
    std::printf("      2048/4: noise %+.2f dB, speech %+.2f dB, output SNR %+.2f dB\n",
                equivalent.noiseAttenuationDb, equivalent.speechGainDb,
                equivalent.outputSnrDb);
    std::printf("      4096/4: noise %+.2f dB, speech %+.2f dB, output SNR %+.2f dB\n",
                largest.noiseAttenuationDb, largest.speechGainDb,
                largest.outputSnrDb);

    report("geometry_quality: 1024/4 improves deterministic output SNR over 256/2",
           lowerLatency.outputSnrDb > current.outputSnrDb);
    const double bestLargeGeometrySpeech = std::max(
        equivalent.speechGainDb, largest.speechGainDb);
    report("geometry_quality: 1024/4 stays within 1 dB of larger geometries",
           lowerLatency.speechGainDb >= bestLargeGeometrySpeech - 1.0);
}

void test_parameter_validation()
{
    SpectralNR nr(1024, 24000, 4);
    nr.setGainMax(std::numeric_limits<float>::quiet_NaN());
    nr.setGainFloor(-1.0f);
    nr.setGainSmooth(std::numeric_limits<float>::infinity());
    nr.setQspp(1.0f);
    nr.setGainMethod(99);
    nr.setNpeMethod(-3);

    report("parameter_validation: non-finite gain cap uses a safe fallback",
           std::abs(nr.gainMax() - 1.0f) < 1e-6f);
    report("parameter_validation: gain floor is clamped",
           std::abs(nr.gainFloor()) < 1e-6f);
    report("parameter_validation: non-finite smoothing uses a safe fallback",
           std::abs(nr.gainSmooth() - 0.85f) < 1e-6f);
    report("parameter_validation: speech prior stays inside the open interval",
           nr.qspp() < 1.0f && nr.qspp() > 0.99f);
    report("parameter_validation: gain method is clamped",
           nr.gainMethod() == 3);
    report("parameter_validation: NPE method is clamped",
           nr.npeMethod() == 0);
}

void test_musical_noise_controls()
{
    const GeometryMetrics floor000 = measureGeometry(
        1024, 4, 0.00f, true, 2, 0.90f);
    const GeometryMetrics floor005 = measureGeometry(
        1024, 4, 0.05f, true, 2, 0.90f);
    const GeometryMetrics floor010 = measureGeometry(
        1024, 4, 0.10f, true, 2, 0.90f);
    const GeometryMetrics floor015 = measureGeometry(
        1024, 4, 0.15f, true, 2, 0.90f);
    const GeometryMetrics aeOff = measureGeometry(
        1024, 4, 0.10f, false, 2, 0.90f);

    std::printf("  floor 0.00: noise %+.2f dB, flatness %.4f\n",
                floor000.noiseAttenuationDb, floor000.residualFlatness);
    std::printf("  floor 0.05: noise %+.2f dB, flatness %.4f\n",
                floor005.noiseAttenuationDb, floor005.residualFlatness);
    std::printf("  floor 0.10: noise %+.2f dB, flatness %.4f\n",
                floor010.noiseAttenuationDb, floor010.residualFlatness);
    std::printf("  floor 0.15: noise %+.2f dB, flatness %.4f\n",
                floor015.noiseAttenuationDb, floor015.residualFlatness);
    std::printf("       AE off: noise %+.2f dB, flatness %.4f\n",
                aeOff.noiseAttenuationDb, aeOff.residualFlatness);

    report("gain_floor: higher settings retain progressively more residual noise",
           floor000.noiseAttenuationDb < floor005.noiseAttenuationDb
               && floor005.noiseAttenuationDb < floor010.noiseAttenuationDb
               && floor010.noiseAttenuationDb < floor015.noiseAttenuationDb);
    report("gain_floor: 0.10 improves residual spectral flatness over 0.05",
           floor010.residualFlatness > floor005.residualFlatness);
    report("adaptive_AE: frequency averaging improves residual spectral flatness",
           floor010.residualFlatness > aeOff.residualFlatness);
}

void test_gain_methods_are_distinct()
{
    const GeometryMetrics linear = measureGeometry(
        1024, 4, 0.10f, true, 0, 0.90f);
    const GeometryMetrics log = measureGeometry(
        1024, 4, 0.10f, true, 1, 0.90f);
    const GeometryMetrics gamma = measureGeometry(
        1024, 4, 0.10f, true, 2, 0.90f);
    const GeometryMetrics trained = measureGeometry(
        1024, 4, 0.10f, true, 3, 0.90f);
    const GeometryMetrics legacyGamma = measureGeometry(
        1024, 4, 0.10f, true, 2, 0.90f, 0, true);

    const GeometryMetrics methods[] = {linear, log, gamma, trained};
    const char* names[] = {"Linear", "Log", "Gamma", "Trained"};
    double minNoiseDb = methods[0].noiseAttenuationDb;
    double maxNoiseDb = methods[0].noiseAttenuationDb;
    double minSpeechDb = methods[0].speechGainDb;
    double maxSpeechDb = methods[0].speechGainDb;
    for (int i = 0; i < 4; ++i) {
        std::printf("  %7s: noise %+.2f dB, speech %+.2f dB, output SNR %+.2f dB\n",
                    names[i], methods[i].noiseAttenuationDb,
                    methods[i].speechGainDb, methods[i].outputSnrDb);
        minNoiseDb = std::min(minNoiseDb, methods[i].noiseAttenuationDb);
        maxNoiseDb = std::max(maxNoiseDb, methods[i].noiseAttenuationDb);
        minSpeechDb = std::min(minSpeechDb, methods[i].speechGainDb);
        maxSpeechDb = std::max(maxSpeechDb, methods[i].speechGainDb);
    }

    report("gain_methods: selections produce measurably different masks",
           maxNoiseDb - minNoiseDb > 0.5 || maxSpeechDb - minSpeechDb > 0.5);
    report("gain_methods: Gamma table path differs from Gaussian-linear path",
           std::abs(gamma.noiseAttenuationDb - linear.noiseAttenuationDb) > 0.25
               || std::abs(gamma.speechGainDb - linear.speechGainDb) > 0.25);
    report("gain_methods: original comparison mode restores legacy method 2",
           std::abs(gamma.noiseAttenuationDb
                    - legacyGamma.noiseAttenuationDb) > 0.25
               || std::abs(gamma.speechGainDb
                           - legacyGamma.speechGainDb) > 0.25);
}

void test_voice_threshold_adjusts_gamma_prior()
{
    const GeometryMetrics threshold020 = measureGeometry(
        1024, 4, 0.00f, true, 2, 0.90f, 0, false, 0.20f);
    const GeometryMetrics threshold035 = measureGeometry(
        1024, 4, 0.00f, true, 2, 0.90f, 0, false, 0.35f);
    const GeometryMetrics threshold050 = measureGeometry(
        1024, 4, 0.00f, true, 2, 0.90f, 0, false, 0.50f);

    std::printf(" threshold 0.20: noise %+.2f dB, speech %+.2f dB\n",
                threshold020.noiseAttenuationDb, threshold020.speechGainDb);
    std::printf(" threshold 0.35: noise %+.2f dB, speech %+.2f dB\n",
                threshold035.noiseAttenuationDb, threshold035.speechGainDb);
    std::printf(" threshold 0.50: noise %+.2f dB, speech %+.2f dB\n",
                threshold050.noiseAttenuationDb, threshold050.speechGainDb);
    report("voice_threshold: higher values progressively increase Gamma suppression",
           threshold035.noiseAttenuationDb
                   < threshold020.noiseAttenuationDb - 0.25
               && threshold050.noiseAttenuationDb
                   < threshold035.noiseAttenuationDb - 0.25);
}

void test_npe_methods_are_distinct()
{
    const GeometryMetrics osms = measureGeometry(
        1024, 4, 0.10f, true, 0, 0.90f, 0);
    const GeometryMetrics mmse = measureGeometry(
        1024, 4, 0.10f, true, 0, 0.90f, 1);
    const GeometryMetrics nstat = measureGeometry(
        1024, 4, 0.10f, true, 0, 0.90f, 2);

    const GeometryMetrics methods[] = {osms, mmse, nstat};
    const char* names[] = {"OSMS", "MMSE", "NSTAT"};
    double minNoiseDb = methods[0].noiseAttenuationDb;
    double maxNoiseDb = methods[0].noiseAttenuationDb;
    double minSpeechDb = methods[0].speechGainDb;
    double maxSpeechDb = methods[0].speechGainDb;
    for (int i = 0; i < 3; ++i) {
        std::printf("  %5s: noise %+.2f dB, speech %+.2f dB, output SNR %+.2f dB\n",
                    names[i], methods[i].noiseAttenuationDb,
                    methods[i].speechGainDb, methods[i].outputSnrDb);
        minNoiseDb = std::min(minNoiseDb, methods[i].noiseAttenuationDb);
        maxNoiseDb = std::max(maxNoiseDb, methods[i].noiseAttenuationDb);
        minSpeechDb = std::min(minSpeechDb, methods[i].speechGainDb);
        maxSpeechDb = std::max(maxSpeechDb, methods[i].speechGainDb);
    }

    report("npe_methods: selections produce measurably different estimates",
           maxNoiseDb - minNoiseDb > 0.5 || maxSpeechDb - minSpeechDb > 0.5);
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

double outputRmsDbfs(const std::vector<float>& output,
                     int firstSample,
                     int lastSample,
                     int latencySamples)
{
    double energy = 0.0;
    for (int i = firstSample; i < lastSample; ++i) {
        energy += static_cast<double>(output[i + latencySamples])
                * output[i + latencySamples];
    }
    const double meanEnergy = energy / std::max(1, lastSample - firstSample);
    return 10.0 * std::log10(std::max(meanEnergy, 1e-20));
}

void test_post_speech_noise_reacquisition()
{
    // Radio AGC commonly exposes a louder broadband noise floor as speech
    // ends. OSMS should recognize that speech-to-noise transition at its next
    // guarded sub-window; otherwise the noise audibly blooms, then fades only
    // when the full minimum-statistics window rotates out the stale floor.
    constexpr int sampleRate = 24000;
    constexpr int fftSize = 1024;
    constexpr int speechStart = 3 * sampleRate;
    constexpr int speechEnd = 5 * sampleRate;
    constexpr int totalSamples = 8 * sampleRate;
    std::vector<float> input(totalSamples);

    std::uint32_t randomState = 0x62726561u;
    for (int i = 0; i < totalSamples; ++i) {
        randomState = 1664525u * randomState + 1013904223u;
        const double white =
            2.0 * (static_cast<double>(randomState) / 4294967295.0) - 1.0;
        // A 6 dB AGC noise-floor rise (2x amplitude, 4x power) is large enough
        // to expose a stale estimate. A sustained broadband rise should be
        // adopted at a sub-window boundary rather than after the full window.
        const double noiseAmplitude = i < speechEnd ? 0.035 : 0.070;
        double sample = noiseAmplitude * white;
        if (i >= speechStart && i < speechEnd) {
            const double t = static_cast<double>(i - speechStart) / sampleRate;
            const double envelope = 0.55 + 0.45 * std::sin(
                2.0 * std::numbers::pi * 2.4 * t)
                                      * std::sin(
                2.0 * std::numbers::pi * 2.4 * t);
            sample += envelope * (
                0.26 * std::sin(2.0 * std::numbers::pi * 180.0 * t)
              + 0.12 * std::sin(2.0 * std::numbers::pi * 720.0 * t)
              + 0.06 * std::sin(2.0 * std::numbers::pi * 1440.0 * t));
        }
        input[i] = static_cast<float>(sample);
    }

    const std::vector<float> output = processWithGeometry(
        input, fftSize, 4, 73, 0.00f, true, 2, 0.90f, 0, false, 0.35f);
    const std::vector<float> defaultFloorOutput = processWithGeometry(
        input, fftSize, 4, 73, 0.10f, true, 2, 0.90f, 0, false, 0.35f);
    const double immediateGainDb = rmsGainDb(
        input, output,
        speechEnd + sampleRate / 10,
        speechEnd + 3 * sampleRate / 10,
        fftSize);
    const double earlyGainDb = rmsGainDb(
        input, output,
        speechEnd + 3 * sampleRate / 10,
        speechEnd + sampleRate / 2,
        fftSize);
    const double reacquiredGainDb = rmsGainDb(
        input, output,
        speechEnd + 3 * sampleRate / 4,
        speechEnd + sampleRate,
        fftSize);
    const double settledGainDb = rmsGainDb(
        input, output,
        speechEnd + 2 * sampleRate,
        speechEnd + 5 * sampleRate / 2,
        fftSize);
    const double remainingBloomDb = reacquiredGainDb - settledGainDb;
    const double defaultFloorImmediateGainDb = rmsGainDb(
        input, defaultFloorOutput,
        speechEnd + sampleRate / 10,
        speechEnd + 3 * sampleRate / 10,
        fftSize);
    const double defaultFloorSettledGainDb = rmsGainDb(
        input, defaultFloorOutput,
        speechEnd + 2 * sampleRate,
        speechEnd + 5 * sampleRate / 2,
        fftSize);

    std::printf(" post-speech: 0.10-0.30 s %+.2f dB, 0.30-0.50 s %+.2f dB, "
                "at 0.75 s %+.2f dB, settled %+.2f dB, remaining bloom %.2f dB\n",
                immediateGainDb, earlyGainDb, reacquiredGainDb,
                settledGainDb, remainingBloomDb);
    std::printf(" default floor: early %+.2f dB, settled %+.2f dB\n",
                defaultFloorImmediateGainDb, defaultFloorSettledGainDb);
    report("post_speech_reacquisition: no brief release spike",
           immediateGainDb - settledGainDb < 3.0
               && earlyGainDb - settledGainDb < 3.0);
    report("post_speech_reacquisition: broadband floor is reacquired within one second",
           remainingBloomDb < 3.0);
    report("post_speech_reacquisition: default floor has no brief release spike",
           defaultFloorImmediateGainDb - defaultFloorSettledGainDb < 3.0);
}

void test_strong_speech_release_memory()
{
    // Keep the receiver noise floor constant so this isolates decision-directed
    // gain memory from the separate AGC/noise-floor-rise case above. A strong
    // voice must not leave speech-open bins passing extra static for seconds.
    constexpr int sampleRate = 24000;
    constexpr int fftSize = 1024;
    constexpr int speechStart = 4 * sampleRate;
    constexpr int speechEnd = 6 * sampleRate;
    constexpr int totalSamples = 10 * sampleRate;
    std::vector<float> input(totalSamples);
    std::uint32_t randomState = 0x766f6963u;
    for (int i = 0; i < totalSamples; ++i) {
        randomState = 1664525u * randomState + 1013904223u;
        const double white =
            2.0 * static_cast<double>(randomState) / 4294967295.0 - 1.0;
        double sample = 0.035 * white;
        if (i >= speechStart && i < speechEnd) {
            const double t = static_cast<double>(i - speechStart) / sampleRate;
            sample += 0.60 * std::sin(2.0 * std::numbers::pi * 180.0 * t)
                    + 0.28 * std::sin(2.0 * std::numbers::pi * 540.0 * t)
                    + 0.16 * std::sin(2.0 * std::numbers::pi * 1080.0 * t)
                    + 0.10 * std::sin(2.0 * std::numbers::pi * 2160.0 * t);
        }
        input[i] = static_cast<float>(sample);
    }

    const std::vector<float> output = processWithGeometry(
        input, fftSize, 4, 73, 0.10f, true, 2, 0.90f, 0, false, 0.35f);
    const double baselineGainDb = rmsGainDb(
        input, output, 3 * sampleRate, speechStart - sampleRate / 4,
        fftSize);
    const double immediateGainDb = rmsGainDb(
        input, output,
        speechEnd + sampleRate / 10,
        speechEnd + 3 * sampleRate / 10,
        fftSize);
    const double halfSecondGainDb = rmsGainDb(
        input, output,
        speechEnd + sampleRate / 2,
        speechEnd + 4 * sampleRate / 5,
        fftSize);
    const double settledGainDb = rmsGainDb(
        input, output,
        speechEnd + 2 * sampleRate,
        speechEnd + 5 * sampleRate / 2,
        fftSize);

    std::printf(" strong release: baseline %+.2f dB, 0.10-0.30 s %+.2f dB, "
                "0.50-0.80 s %+.2f dB, settled %+.2f dB\n",
                baselineGainDb, immediateGainDb, halfSecondGainDb,
                settledGainDb);
    report("post_speech_memory: strong voice does not leave a multi-second static tail",
           immediateGainDb - settledGainDb < 3.0
               && halfSecondGainDb - settledGainDb < 2.0);
    report("post_speech_memory: suppression returns close to the pre-speech baseline",
           immediateGainDb - baselineGainDb < 3.0);
}

void test_radio_agc_release_static_level()
{
    // A strong received voice can temporarily lower the radio's AGC gain. As
    // the AGC releases, the raw broadband floor rises and then decays over a
    // couple of seconds. NR2 should hold the audible residual near its prior
    // level instead of applying a fixed gain floor to that transient rise.
    constexpr int sampleRate = 24000;
    constexpr int fftSize = 1024;
    constexpr int speechStart = 3 * sampleRate;
    constexpr int speechEnd = 5 * sampleRate;
    constexpr int totalSamples = 9 * sampleRate;
    std::vector<float> input(totalSamples);
    std::uint32_t randomState = 0x61676372u;
    for (int i = 0; i < totalSamples; ++i) {
        randomState = 1664525u * randomState + 1013904223u;
        const double white =
            2.0 * static_cast<double>(randomState) / 4294967295.0 - 1.0;
        double noiseAmplitude = 0.035;
        if (i >= speechEnd) {
            const double releaseSeconds =
                static_cast<double>(i - speechEnd) / sampleRate;
            noiseAmplitude *= 1.0 + std::exp(-releaseSeconds / 0.70);
        }
        double sample = noiseAmplitude * white;
        if (i >= speechStart && i < speechEnd) {
            const double t = static_cast<double>(i - speechStart) / sampleRate;
            sample += 0.55 * std::sin(2.0 * std::numbers::pi * 190.0 * t)
                    + 0.25 * std::sin(2.0 * std::numbers::pi * 760.0 * t)
                    + 0.12 * std::sin(2.0 * std::numbers::pi * 1520.0 * t);
        }
        input[i] = static_cast<float>(sample);
    }

    const std::vector<float> output = processWithGeometry(
        input, fftSize, 4, 73, 0.10f, true, 2, 0.90f, 0, false, 0.35f);
    const double baselineDbfs = outputRmsDbfs(
        output, 2 * sampleRate, speechStart - sampleRate / 4, fftSize);
    const double immediateDbfs = outputRmsDbfs(
        output,
        speechEnd + sampleRate / 10,
        speechEnd + 3 * sampleRate / 10,
        fftSize);
    const double oneSecondDbfs = outputRmsDbfs(
        output,
        speechEnd + 7 * sampleRate / 10,
        speechEnd + sampleRate,
        fftSize);
    const double settledDbfs = outputRmsDbfs(
        output,
        speechEnd + 5 * sampleRate / 2,
        speechEnd + 3 * sampleRate,
        fftSize);

    std::printf(" AGC release residual: baseline %.2f dBFS, 0.10-0.30 s %.2f dBFS, "
                "0.70-1.00 s %.2f dBFS, settled %.2f dBFS\n",
                baselineDbfs, immediateDbfs, oneSecondDbfs, settledDbfs);
    report("post_speech_agc: residual static does not surge after strong speech",
           immediateDbfs - baselineDbfs < 2.0
               && oneSecondDbfs - baselineDbfs < 2.0);
    report("post_speech_agc: settled residual returns to baseline",
           std::abs(settledDbfs - baselineDbfs) < 1.0);
}

void test_wideband_strong_station_release()
{
    // A strong communications signal can occupy most of the passband once
    // receiver filtering and AGC are included. Model that with a dense voiced
    // harmonic spectrum, followed by a large broadband AGC-release burst that
    // decays over roughly two seconds. This specifically covers the live-radio
    // report that the static stays loud for several seconds after speech ends.
    constexpr int sampleRate = 24000;
    constexpr int fftSize = 1024;
    constexpr int speechStart = 3 * sampleRate;
    constexpr int speechEnd = 5 * sampleRate;
    constexpr int totalSamples = 10 * sampleRate;
    std::vector<float> input(totalSamples);
    std::uint32_t randomState = 0x77696465u;
    for (int i = 0; i < totalSamples; ++i) {
        randomState = 1664525u * randomState + 1013904223u;
        const double white =
            2.0 * static_cast<double>(randomState) / 4294967295.0 - 1.0;
        double noiseAmplitude = 0.025;
        if (i >= speechEnd) {
            const double releaseSeconds =
                static_cast<double>(i - speechEnd) / sampleRate;
            noiseAmplitude *= 1.0 + 3.0 * std::exp(-releaseSeconds / 1.25);
        }
        double sample = noiseAmplitude * white;
        if (i >= speechStart && i < speechEnd) {
            const double t = static_cast<double>(i - speechStart) / sampleRate;
            const double f0 = 105.0 + 15.0 * std::sin(
                2.0 * std::numbers::pi * 0.7 * t);
            const double envelope = 0.65 + 0.35 * std::sin(
                2.0 * std::numbers::pi * 2.9 * t)
                                      * std::sin(
                2.0 * std::numbers::pi * 2.9 * t);
            for (int harmonic = 2; harmonic * f0 < 4200.0; ++harmonic) {
                sample += envelope * 0.16 / std::sqrt(harmonic)
                        * std::sin(2.0 * std::numbers::pi
                                   * harmonic * f0 * t);
            }
        }
        input[i] = static_cast<float>(sample);
    }

    const std::vector<float> output = processWithGeometry(
        input, fftSize, 4, 73, 0.10f, true, 2, 0.90f, 0, false, 0.35f);
    const double speechFlatness = meanSpectralFlatness(
        input, speechStart + sampleRate, speechEnd);
    const double releaseFlatness = meanSpectralFlatness(
        input, speechEnd, speechEnd + sampleRate);
    const double baselineDbfs = outputRmsDbfs(
        output, 2 * sampleRate, speechStart - sampleRate / 4, fftSize);
    const double immediateDbfs = outputRmsDbfs(
        output,
        speechEnd + sampleRate / 10,
        speechEnd + 35 * sampleRate / 100,
        fftSize);
    const double oneSecondDbfs = outputRmsDbfs(
        output,
        speechEnd + sampleRate,
        speechEnd + 3 * sampleRate / 2,
        fftSize);
    const double settledDbfs = outputRmsDbfs(
        output,
        speechEnd + 7 * sampleRate / 2,
        speechEnd + 4 * sampleRate,
        fftSize);

    std::printf(" wideband input flatness: speech %.4f, release %.4f\n",
                speechFlatness, releaseFlatness);
    std::printf(" wideband release residual: baseline %.2f dBFS, "
                "0.10-0.35 s %.2f dBFS, 1.0-1.5 s %.2f dBFS, "
                "settled %.2f dBFS\n",
                baselineDbfs, immediateDbfs, oneSecondDbfs, settledDbfs);
    report("wideband_release: no multi-second static surge after a strong station",
           immediateDbfs - baselineDbfs < 2.0
               && oneSecondDbfs - baselineDbfs < 2.0);
    report("wideband_release: settled residual returns near baseline",
           std::abs(settledDbfs - baselineDbfs) < 1.5);
}

void test_filtered_receiver_noise_release()
{
    // Receiver audio is sharply band-limited, unlike the full-band white
    // noise used by the broad AGC regression above. Exercise a delayed,
    // two-second AGC floor rise through a 200-3000 Hz approximation so the
    // release detector cannot rely on full-band spectral flatness.
    constexpr int sampleRate = 24000;
    constexpr int fftSize = 1024;
    constexpr int speechStart = 3 * sampleRate;
    constexpr int speechEnd = 5 * sampleRate;
    constexpr int releaseStart = speechEnd + sampleRate / 8;
    constexpr int totalSamples = 10 * sampleRate;
    const double lowPassAlpha = 1.0 - std::exp(
        -2.0 * std::numbers::pi * 3000.0 / sampleRate);
    const double highPassAlpha = 1.0 - std::exp(
        -2.0 * std::numbers::pi * 200.0 / sampleRate);

    std::vector<float> input(totalSamples);
    std::uint32_t randomState = 0x66696c74u;
    double lowPass1 = 0.0;
    double lowPass2 = 0.0;
    double lowPass3 = 0.0;
    double lowPass4 = 0.0;
    double lowFrequency = 0.0;
    for (int i = 0; i < totalSamples; ++i) {
        randomState = 1664525u * randomState + 1013904223u;
        const double white =
            2.0 * static_cast<double>(randomState) / 4294967295.0 - 1.0;
        lowPass1 += lowPassAlpha * (white - lowPass1);
        lowPass2 += lowPassAlpha * (lowPass1 - lowPass2);
        lowPass3 += lowPassAlpha * (lowPass2 - lowPass3);
        lowPass4 += lowPassAlpha * (lowPass3 - lowPass4);
        lowFrequency += highPassAlpha * (lowPass4 - lowFrequency);
        const double bandNoise = 2.5 * (lowPass4 - lowFrequency);

        double noiseAmplitude = 0.025;
        if (i >= releaseStart) {
            const double releaseSeconds =
                static_cast<double>(i - releaseStart) / sampleRate;
            noiseAmplitude *= 1.0 + 3.0 * std::exp(-releaseSeconds / 1.25);
        }
        double sample = noiseAmplitude * bandNoise;
        if (i >= speechStart && i < speechEnd) {
            const double t = static_cast<double>(i - speechStart) / sampleRate;
            const double f0 = 105.0 + 15.0 * std::sin(
                2.0 * std::numbers::pi * 0.7 * t);
            for (int harmonic = 2; harmonic * f0 < 3000.0; ++harmonic) {
                sample += 0.16 / std::sqrt(harmonic)
                        * std::sin(2.0 * std::numbers::pi
                                   * harmonic * f0 * t);
            }
        }
        input[i] = static_cast<float>(sample);
    }

    for (const int npeMethod : {0, 1}) {
        const std::vector<float> output = processWithGeometry(
            input, fftSize, 4, 73, 0.10f, true, 2, 0.90f,
            npeMethod, false, 0.35f);
        const double baselineDbfs = outputRmsDbfs(
            output, 2 * sampleRate, speechStart - sampleRate / 4, fftSize);
        const double immediateDbfs = outputRmsDbfs(
            output,
            speechEnd + sampleRate / 5,
            speechEnd + 45 * sampleRate / 100,
            fftSize);
        const double oneSecondDbfs = outputRmsDbfs(
            output,
            speechEnd + sampleRate,
            speechEnd + 3 * sampleRate / 2,
            fftSize);
        const char* methodName = npeMethod == 0 ? "OSMS" : "MMSE";
        const std::string testName = std::string("filtered_release: ")
                                   + methodName
                                   + " band-limited static does not surge after speech";

        std::printf(" filtered release %s: baseline %.2f dBFS, "
                    "0.20-0.45 s %.2f dBFS, 1.0-1.5 s %.2f dBFS\n",
                    methodName, baselineDbfs, immediateDbfs, oneSecondDbfs);
        report(testName.c_str(),
               immediateDbfs - baselineDbfs < 2.0
                   && oneSecondDbfs - baselineDbfs < 2.0);
    }
}

void test_npe_switch_transients()
{
    constexpr int sampleRate = 24000;
    constexpr int fftSize = 1024;
    constexpr int blockSize = 128;
    double worstDifference = 0.0;
    int worstMethod = -1;
    int worstSwitchSecond = -1;
    double worstAmplitude = 0.0;

    const auto measureSwitch = [&](int method, int switchSecond,
                                   double amplitude) {
        const int switchSample = switchSecond * sampleRate;
        const int totalSamples = (switchSecond + 3) * sampleRate;
        std::vector<float> input(totalSamples);
        std::uint32_t randomState = 0x73776974u
                                  + static_cast<std::uint32_t>(switchSecond * 31)
                                  + static_cast<std::uint32_t>(amplitude * 1000.0);
        for (float& sample : input) {
            randomState = 1664525u * randomState + 1013904223u;
            sample = static_cast<float>(amplitude * (
                2.0 * static_cast<double>(randomState) / 4294967295.0 - 1.0));
        }

        SpectralNR switched(fftSize, sampleRate, 4);
        switched.setGainMethod(2);
        switched.setGainFloor(0.0f);
        switched.setGainSmooth(0.90f);
        SpectralNR reference(fftSize, sampleRate, 4);
        reference.setGainMethod(2);
        reference.setGainFloor(0.0f);
        reference.setGainSmooth(0.90f);
        reference.setNpeMethod(method);
        std::vector<float> switchedOutput(input.size());
        std::vector<float> referenceOutput(input.size());
        for (int offset = 0; offset < totalSamples; offset += blockSize) {
            if (offset == switchSample) {
                switched.setNpeMethod(method);
            }
            const int count = std::min(blockSize, totalSamples - offset);
            switched.process(input.data() + offset,
                             switchedOutput.data() + offset, count);
            reference.process(input.data() + offset,
                              referenceOutput.data() + offset, count);
        }
        const double switchedImmediate = rmsGainDb(
            input, switchedOutput,
            switchSample + sampleRate / 10,
            switchSample + 3 * sampleRate / 10,
            fftSize);
        const double referenceImmediate = rmsGainDb(
            input, referenceOutput,
            switchSample + sampleRate / 10,
            switchSample + 3 * sampleRate / 10,
            fftSize);
        return std::pair{switchedImmediate, referenceImmediate};
    };

    for (const int method : {1, 2}) {
        for (const int switchSecond : {2, 3, 4}) {
            for (const double amplitude : {0.02, 0.08}) {
                const auto [switchedImmediate, referenceImmediate] = measureSwitch(
                    method, switchSecond, amplitude);
                const double difference = std::abs(
                    switchedImmediate - referenceImmediate);
                std::printf(" %s switch at %d s, amplitude %.2f: "
                            "switched %+.2f dB, always-selected %+.2f dB, "
                            "delta %.2f dB\n",
                            method == 1 ? "MMSE" : "NSTAT", switchSecond,
                            amplitude, switchedImmediate, referenceImmediate,
                            difference);
                if (difference > worstDifference) {
                    worstDifference = difference;
                    worstMethod = method;
                    worstSwitchSecond = switchSecond;
                    worstAmplitude = amplitude;
                }
            }
        }
    }
    std::printf(" worst live switch: %s at %d s, amplitude %.2f, delta %.2f dB\n",
                worstMethod == 1 ? "MMSE" : "NSTAT", worstSwitchSecond,
                worstAmplitude, worstDifference);
    report("npe_switch: live methods stay warm across timing and level",
           worstDifference < 4.0);
}

double projectedSignalGainDb(const std::vector<float>& clean,
                             const std::vector<float>& output,
                             int firstSample,
                             int lastSample,
                             int latencySamples)
{
    double cleanEnergy = 0.0;
    double outputCleanDot = 0.0;
    for (int i = firstSample; i < lastSample; ++i) {
        cleanEnergy += static_cast<double>(clean[i]) * clean[i];
        outputCleanDot += static_cast<double>(output[i + latencySamples]) * clean[i];
    }
    const double gain = outputCleanDot / std::max(cleanEnergy, 1e-20);
    return 20.0 * std::log10(std::max(std::abs(gain), 1e-20));
}

void test_quick_reply_after_speech_release()
{
    constexpr int sampleRate = 24000;
    constexpr int fftSize = 1024;
    constexpr int firstSpeechStart = 3 * sampleRate;
    constexpr int firstSpeechEnd = 5 * sampleRate;
    constexpr int secondSpeechStart = firstSpeechEnd + 3 * sampleRate / 10;
    constexpr int secondSpeechEnd = 7 * sampleRate;
    constexpr int totalSamples = 8 * sampleRate;
    std::vector<float> clean(totalSamples, 0.0f);
    std::vector<float> input(totalSamples, 0.0f);
    std::uint32_t randomState = 0x71756963u;
    double phase = 0.0;
    for (int i = 0; i < totalSamples; ++i) {
        randomState = 1664525u * randomState + 1013904223u;
        const double white =
            2.0 * static_cast<double>(randomState) / 4294967295.0 - 1.0;
        const bool firstSpeech = i >= firstSpeechStart && i < firstSpeechEnd;
        const bool secondSpeech = i >= secondSpeechStart && i < secondSpeechEnd;
        const double noiseAmplitude =
            i >= firstSpeechEnd && i < secondSpeechStart ? 0.070 : 0.035;
        if (firstSpeech || secondSpeech) {
            phase += 2.0 * std::numbers::pi * 175.0 / sampleRate;
            clean[i] = static_cast<float>(
                0.25 * std::sin(phase)
              + 0.12 * std::sin(2.0 * phase)
              + 0.06 * std::sin(4.0 * phase));
        }
        input[i] = clean[i] + static_cast<float>(noiseAmplitude * white);
    }

    const std::vector<float> output = processWithGeometry(
        input, fftSize, 4, 73, 0.00f, true, 2, 0.90f, 0, false, 0.35f);
    const double firstGainDb = projectedSignalGainDb(
        clean, output,
        firstSpeechStart + sampleRate,
        firstSpeechEnd - sampleRate / 5,
        fftSize);
    const double replyGainDb = projectedSignalGainDb(
        clean, output,
        secondSpeechStart + sampleRate / 10,
        secondSpeechStart + 4 * sampleRate / 10,
        fftSize);
    std::printf(" quick reply: first speech %+.2f dB, reply onset %+.2f dB\n",
                firstGainDb, replyGainDb);
    report("post_speech_release: a quick reply clears the release guard",
           replyGainDb >= firstGainDb - 3.0);
}

void test_weak_reply_after_speech_release()
{
    constexpr int sampleRate = 24000;
    constexpr int fftSize = 1024;
    constexpr int firstSpeechStart = 3 * sampleRate;
    constexpr int firstSpeechEnd = 5 * sampleRate;
    constexpr int secondSpeechStart = firstSpeechEnd + 3 * sampleRate / 10;
    constexpr int secondSpeechEnd = 6 * sampleRate;
    constexpr int totalSamples = 7 * sampleRate;
    std::vector<float> clean(totalSamples, 0.0f);
    std::vector<float> input(totalSamples, 0.0f);
    std::vector<float> steadyNoiseInput(totalSamples, 0.0f);
    std::uint32_t randomState = 0x7765616bu;
    double phase = 0.0;
    for (int i = 0; i < totalSamples; ++i) {
        randomState = 1664525u * randomState + 1013904223u;
        const double white =
            2.0 * static_cast<double>(randomState) / 4294967295.0 - 1.0;
        const bool firstSpeech = i >= firstSpeechStart && i < firstSpeechEnd;
        const bool secondSpeech = i >= secondSpeechStart && i < secondSpeechEnd;
        const double noiseAmplitude = i >= firstSpeechEnd ? 0.070 : 0.035;
        if (firstSpeech || secondSpeech) {
            phase += 2.0 * std::numbers::pi * 175.0 / sampleRate;
            const double speechScale = secondSpeech ? 0.18 : 1.0;
            clean[i] = static_cast<float>(speechScale * (
                0.25 * std::sin(phase)
              + 0.12 * std::sin(2.0 * phase)
              + 0.06 * std::sin(4.0 * phase)));
        }
        input[i] = clean[i] + static_cast<float>(noiseAmplitude * white);
        steadyNoiseInput[i] = static_cast<float>(0.070 * white)
                            + (secondSpeech ? clean[i] : 0.0f);
    }

    const std::vector<float> output = processWithGeometry(
        input, fftSize, 4, 73, 0.00f, true, 2, 0.90f, 0, false, 0.35f);
    const std::vector<float> steadyNoiseOutput = processWithGeometry(
        steadyNoiseInput, fftSize, 4, 73, 0.00f, true, 2, 0.90f,
        0, false, 0.35f);
    const double firstGainDb = projectedSignalGainDb(
        clean, output,
        firstSpeechStart + sampleRate,
        firstSpeechEnd - sampleRate / 5,
        fftSize);
    const double replyGainDb = projectedSignalGainDb(
        clean, output,
        secondSpeechStart + sampleRate / 10,
        secondSpeechStart + 4 * sampleRate / 10,
        fftSize);
    const double steadyNoiseGainDb = projectedSignalGainDb(
        clean, steadyNoiseOutput,
        secondSpeechStart + sampleRate / 10,
        secondSpeechStart + 4 * sampleRate / 10,
        fftSize);
    std::printf(" weak reply: strong speech %+.2f dB, reply %+.2f dB, "
                "steady-noise control %+.2f dB\n",
                firstGainDb, replyGainDb, steadyNoiseGainDb);
    report("post_speech_release: release does not add weak-speech attenuation",
           replyGainDb >= steadyNoiseGainDb - 3.0);
}

std::vector<float> processStereoSharedMaskWithBlockSize(
    const std::vector<float>& input,
    int blockFrames)
{
    SpectralNR nr(1024, 24000, 4);
    nr.setGainMethod(2);

    std::vector<float> output(input.size());
    int offsetFrames = 0;
    const int totalFrames = static_cast<int>(input.size() / 2);
    while (offsetFrames < totalFrames) {
        const int count = std::min(blockFrames, totalFrames - offsetFrames);
        nr.processStereoSharedMask(input.data() + (2 * offsetFrames),
                                   output.data() + (2 * offsetFrames),
                                   count);
        offsetFrames += count;
    }
    return output;
}

std::pair<double, double> stereoRmsAfter(const std::vector<float>& interleaved,
                                         int startFrame)
{
    double leftSum = 0.0;
    double rightSum = 0.0;
    int count = 0;
    const int totalFrames = static_cast<int>(interleaved.size() / 2);
    for (int frame = startFrame; frame < totalFrames; ++frame) {
        const double left = interleaved[2 * frame];
        const double right = interleaved[2 * frame + 1];
        leftSum += left * left;
        rightSum += right * right;
        ++count;
    }
    return {
        std::sqrt(leftSum / std::max(count, 1)),
        std::sqrt(rightSum / std::max(count, 1)),
    };
}

void test_block_size_invariance()
{
    // KiwiSDR audio arrives in packet-sized bursts, while native Flex RX audio
    // typically reaches NR2 at the 128-sample hop cadence. NR2 must produce the
    // same stream either way; otherwise burst-sized calls can wrap the OLA ring
    // and sound like rapid periodic audio gaps.
    const std::vector<float> input = buildSyntheticAudio(24000);
    const std::vector<float> hopOutput = processWithBlockSize(input, 128);
    const std::vector<float> packetOutput = processWithBlockSize(input, 1024);
    const std::vector<float> shortOutput = processWithBlockSize(input, 73);
    const std::vector<float> equivalentHopOutput =
        processWithGeometry(input, 1024, 4, 256);
    const std::vector<float> equivalentShortOutput =
        processWithGeometry(input, 1024, 4, 73);

    double packetMaxAbsDiff = 0.0;
    double shortMaxAbsDiff = 0.0;
    double equivalentMaxAbsDiff = 0.0;
    for (std::size_t i = 0; i < hopOutput.size(); ++i) {
        packetMaxAbsDiff = std::max(
            packetMaxAbsDiff,
            static_cast<double>(std::abs(hopOutput[i] - packetOutput[i])));
        shortMaxAbsDiff = std::max(
            shortMaxAbsDiff,
            static_cast<double>(std::abs(hopOutput[i] - shortOutput[i])));
        equivalentMaxAbsDiff = std::max(
            equivalentMaxAbsDiff,
            static_cast<double>(std::abs(
                equivalentHopOutput[i] - equivalentShortOutput[i])));
    }

    char detail[96];
    std::snprintf(detail, sizeof(detail), " (max abs diff: %.3e)", packetMaxAbsDiff);
    report("block_size_invariance: 1024-sample packets match 128-sample hops",
           packetMaxAbsDiff < 1e-7, detail);

    std::snprintf(detail, sizeof(detail), " (max abs diff: %.3e)", shortMaxAbsDiff);
    report("block_size_invariance: 73-sample callbacks match 128-sample hops",
           shortMaxAbsDiff < 1e-7, detail);

    std::snprintf(detail, sizeof(detail),
                  " (max abs diff: %.3e)", equivalentMaxAbsDiff);
    report("block_size_invariance: 1024/4 geometry is callback-size invariant",
           equivalentMaxAbsDiff < 1e-7, detail);
}

void test_stereo_shared_mask_preserves_balance()
{
    // Flex remote_audio_rx is one radio-mixed stereo stream.  NR2 should use a
    // shared noise estimate, but must not collapse the stream to mono and then
    // rebuild it with one active-slice pan value (#4035).
    constexpr int sampleRate = 24000;
    constexpr int frames = sampleRate;
    std::vector<float> input(frames * 2);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const float signal = static_cast<float>(
            0.34 * std::sin(2.0 * std::numbers::pi * 720.0 * t)
          + 0.13 * std::sin(2.0 * std::numbers::pi * 1740.0 * t)
          + 0.03 * std::sin(2.0 * std::numbers::pi * 43.0 * t));
        input[2 * i] = 0.80f * signal;
        input[2 * i + 1] = 0.20f * signal;
    }

    const std::vector<float> output =
        processStereoSharedMaskWithBlockSize(input, 1024);
    const std::vector<float> shortOutput =
        processStereoSharedMaskWithBlockSize(input, 73);

    double maxAbsDiff = 0.0;
    for (std::size_t i = 0; i < output.size(); ++i) {
        maxAbsDiff = std::max(
            maxAbsDiff,
            static_cast<double>(std::abs(output[i] - shortOutput[i])));
    }
    char detail[128];
    std::snprintf(detail, sizeof(detail),
                  " (max abs diff: %.3e)", maxAbsDiff);
    report("stereo_shared_mask: output is callback-size invariant",
           maxAbsDiff < 1e-7, detail);

    constexpr int discardFrames = 4096;
    const auto [inLeft, inRight] = stereoRmsAfter(input, discardFrames);
    const auto [outLeft, outRight] = stereoRmsAfter(output, discardFrames);
    const double inRatio = inLeft / std::max(inRight, 1e-12);
    const double outRatio = outLeft / std::max(outRight, 1e-12);
    const double ratioError = std::abs(outRatio - inRatio);

    std::snprintf(detail, sizeof(detail),
                  " (input L/R %.4f, output L/R %.4f, err %.3e)",
                  inRatio, outRatio, ratioError);
    report("stereo_shared_mask: preserves unbalanced L/R energy",
           ratioError < 1e-3, detail);

    std::snprintf(detail, sizeof(detail),
                  " (output L %.6f, R %.6f)", outLeft, outRight);
    report("stereo_shared_mask: output remains non-silent",
           outLeft > 1e-5 && outRight > 1e-5, detail);
}

void test_gain_formula_extreme_v()
{
    // Directly verify the Ephraim-Malah gain formula at v values that caused
    // NaN with the old unscaled Bessel functions (v > 1420).
    //
    // With the fix, bessI0e(v/2) / bessI1e(v/2) are bounded by 1/sqrt(v/2) * poly
    // for large v, so the gain expression is finite and approaches ~1.0 for very
    // large v (pass-through for a strong signal).
    //
    // Constants match SpectralNR internals:
    constexpr double gf1p5    = 0.8862269254527580; // sqrt(pi)/2 = Gamma(3/2)
    constexpr double gammaStress = 1e4;
    constexpr double EpsFloor = 1e-300;

    // Test at v values straddling and far above the old overflow threshold.
    const std::vector<double> v_values = {1419.0, 1420.0, 1421.0, 2000.0, 5000.0, 10000.0};

    for (double v : v_values) {
        // Deliberately stress the formula beyond the runtime SNR cap.
        double gamma = gammaStress;

        // Compute the gain formula directly using the local bessI0e/bessI1e.
        double gain = gf1p5 * std::sqrt(v) / std::max(gamma, EpsFloor)
                    * ((1.0 + v) * bessI0e(0.5 * v) + v * bessI1e(0.5 * v));

        char name[128];
        std::snprintf(name, sizeof(name),
                      "gain_formula finite at v=%.0f", v);
        report(name, std::isfinite(gain));

        std::snprintf(name, sizeof(name),
                      "gain_formula >= 0 at v=%.0f", v);
        report(name, gain >= 0.0);

        std::snprintf(name, sizeof(name),
                      "gain_formula not NaN-clamp sentinel (0.01) at v=%.0f", v);
        report(name, gain > 0.05);   // 0.01 sentinel is well below this

        // For large v, the pre-clamp formula approaches v/gamma ≈ 1.0; small
        // floating-point overshoot is expected and clamped by
        // computeGainLinear().
        std::snprintf(name, sizeof(name),
                      "gain_formula <= 1.1 (pre-clamp, v=%.0f, gain=%.6f)", v, gain);
        report(name, gain <= 1.1);
    }
}

} // namespace

int main()
{
    std::printf("\n=== spectral_nr_test ===\n\n");

    std::printf("-- Bessel finiteness (old threshold: v=1420) --\n");
    test_bessel_finiteness();

    std::printf("\n-- Bessel mathematical equivalence (x <= 50) --\n");
    test_bessel_equivalence();

    std::printf("\n-- Gain finiteness (1000 hops, full-scale 1 kHz sine) --\n");
    test_gain_finiteness();

    std::printf("\n-- NR2 parameter boundary validation --\n");
    test_parameter_validation();

    std::printf("\n-- Block-size invariance (Kiwi packet cadence regression) --\n");
    test_block_size_invariance();

    std::printf("\n-- NR2 FFT geometry quality comparison --\n");
    test_geometry_quality_comparison();

    std::printf("\n-- NR2 default parameter profiles --\n");
    test_default_parameter_profiles();

    std::printf("\n-- NR2 musical-noise controls --\n");
    test_musical_noise_controls();

    std::printf("\n-- NR2 gain-method comparison --\n");
    test_gain_methods_are_distinct();

    std::printf("\n-- NR2 Gamma voice-threshold prior --\n");
    test_voice_threshold_adjusts_gamma_prior();

    std::printf("\n-- NR2 NPE-method comparison --\n");
    test_npe_methods_are_distinct();

    std::printf("\n-- NR2 post-speech noise-floor reacquisition --\n");
    test_post_speech_noise_reacquisition();

    std::printf("\n-- NR2 strong-speech decision-directed release --\n");
    test_strong_speech_release_memory();

    std::printf("\n-- NR2 receiver-AGC release residual level --\n");
    test_radio_agc_release_static_level();

    std::printf("\n-- NR2 wideband strong-station release --\n");
    test_wideband_strong_station_release();

    std::printf("\n-- NR2 filtered receiver-noise release --\n");
    test_filtered_receiver_noise_release();

    std::printf("\n-- NR2 live NPE switching --\n");
    test_npe_switch_transients();

    std::printf("\n-- NR2 quick reply after speech release --\n");
    test_quick_reply_after_speech_release();
    test_weak_reply_after_speech_release();

    std::printf("\n-- Stereo shared-mask balance preservation (#4035) --\n");
    test_stereo_shared_mask_preserves_balance();

    std::printf("\n-- Gain formula at extreme v (NaN-clamp regression) --\n");
    test_gain_formula_extreme_v();

    std::printf("\n%s — %d test(s) failed\n\n",
                g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
