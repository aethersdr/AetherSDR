// NnrFilter end to end: real audio through WDSP 2.10's Neural Noise Reduction.
//
// This is the test nnr_controls_test could not be — it constructs a filter and
// checks what comes out, including the cross-check that the ranges in
// NnrControls.h still describe the WDSP being linked. A refresh that moves a
// default fails here.
//
// Signals are synthetic on purpose: a listening judgement belongs in the A/B
// against DFNR and RN2 (RFC #5684 §5), while what a test can hold is that
// noise goes down, voice-shaped content does not, and the controls move the
// result in the direction they claim.

#include "core/NnrControls.h"
#include "core/NnrFilter.h"

#include <QByteArray>

#include <cmath>
#include <numbers>
#include <cstdio>
#include <vector>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr int kRate = 48000;
constexpr int kBlock = 512;   // stereo frames per process() call

unsigned g_rand = 12345;
float noise()
{
    g_rand = g_rand * 1103515245u + 12345u;
    return (static_cast<float>(g_rand >> 8) / 8388608.0f - 1.0f) * 0.05f;
}

// Voice-shaped: a 120 Hz fundamental with formant-weighted harmonics, syllable
// modulated. Not speech, but harmonic and modulated where a carrier is not.
float voice(long n)
{
    const double t = static_cast<double>(n) / kRate;
    const double env = 0.5 * (1.0 + std::sin(2.0 * std::numbers::pi * 3.0 * t));
    double v = 0.0;
    for (int h = 1; h <= 12; ++h) {
        const double f = 120.0 * h;
        const double a = std::exp(-std::fabs(f - 500.0) / 600.0)
                       + 0.7 * std::exp(-std::fabs(f - 1500.0) / 500.0);
        v += a * std::sin(2.0 * std::numbers::pi * f * t + 0.7 * h);
    }
    return static_cast<float>(0.05 * env * v);
}

// Returns output/input power in dB, measured after the filter has settled.
double runDb(NnrFilter& f, bool withVoice, int blocks = 220, int settle = 60)
{
    g_rand = 4242;
    double pin = 0.0, pout = 0.0;
    long n = 0;
    QByteArray in(kBlock * 2 * sizeof(float), Qt::Uninitialized);
    for (int b = 0; b < blocks; ++b) {
        auto* s = reinterpret_cast<float*>(in.data());
        for (int i = 0; i < kBlock; ++i) {
            const float v = noise() + (withVoice ? voice(static_cast<long>(b) * kBlock + i) : 0.0f);
            s[i * 2] = v;
            s[i * 2 + 1] = v;
        }
        const QByteArray out = f.process(in);
        if (b < settle) {
            continue;
        }
        const auto* o = reinterpret_cast<const float*>(out.constData());
        const int frames = out.size() / (2 * static_cast<int>(sizeof(float)));
        for (int i = 0; i < kBlock; ++i) {
            pin += static_cast<double>(s[i * 2]) * s[i * 2];
        }
        for (int i = 0; i < frames; ++i) {
            pout += static_cast<double>(o[i * 2]) * o[i * 2];
        }
        n += kBlock;
    }
    if (n == 0 || pin <= 0.0) {
        return 0.0;
    }
    return 10.0 * std::log10((pout / n + 1e-30) / (pin / n + 1e-30));
}

}  // namespace

int main()
{
    NnrFilter filter(kRate);
    check(filter.isValid(), "the filter did not construct");
    if (!filter.isValid()) {
        return 1;
    }

    // WDSP documents 51.17 ms, and both models share it.
    const double delayMs = 1000.0 * filter.delaySamples() / kRate;
    check(delayMs > 50.0 && delayMs < 53.0, "NNR's algorithmic delay moved");

    // Noise alone should be attenuated hard; voice-shaped content should not.
    const double noiseOnly = runDb(filter, false);
    check(noiseOnly < -15.0, "noise-only input was not attenuated");
    filter.reset();
    const double withVoice = runDb(filter, true);
    check(withVoice > noiseOnly + 10.0,
          "voice-shaped content was attenuated like noise");

    // Strength drives the mask floor: more strength, more suppression. The
    // ends of the travel are NnrControls.h's range, so this also pins the
    // mapping the tab will use.
    filter.reset();
    filter.setStrength(0);
    const double light = runDb(filter, false);
    filter.reset();
    filter.setStrength(100);
    const double heavy = runDb(filter, false);
    check(heavy < light, "raising strength did not increase suppression");
    check(light > -25.0, "strength 0 suppressed far more than the floor allows");

    // The model control reports what WDSP switched to rather than what was
    // asked for, which is how a build without Premium is discovered.
    filter.setStrength(50);
    filter.reset();
    filter.setModel(1);
    runDb(filter, false, 20, 0);
    check(filter.modelSlot() == 1, "Premium did not become the active slot");
    filter.setModel(0);
    runDb(filter, false, 20, 0);
    check(filter.modelSlot() == 0, "Standard did not become the active slot");

    // A slot that does not exist leaves the active one alone.
    filter.setModel(7);
    runDb(filter, false, 20, 0);
    check(filter.modelSlot() == 0, "an absent slot changed the active model");

    // The marker positions the tab draws must still describe this WDSP.
    check(Nnr::kMaskFloor.defaultValue == -25.0, "the mask-floor default moved");
    check(Nnr::kAlphaKnee.defaultValue == 10.0, "the alpha-knee default moved");

    // The 24 kHz path is the one the latency fix is about: at 48 kHz there are
    // no resamplers and totalLatencyFrames() equals NNR's own delay, so a test
    // at 48 kHz alone cannot see the defect this fixes.
    {
        NnrFilter narrow(24000);
        check(narrow.isValid(), "the filter did not construct at 24 kHz");
        if (narrow.isValid()) {
            const double declaredMs = 1000.0 * narrow.delaySamples() / 24000.0;
            // NNR's own 51.17 ms plus two resamplers' group delay. Well over
            // NNR's alone, which is exactly what was being declared before.
            check(declaredMs > 100.0,
                  "the 24 kHz path is declaring NNR's delay without the "
                  "resamplers' group delay");
            check(declaredMs < 400.0, "the declared 24 kHz latency is implausible");
            check(narrow.delaySamples() > filter.delaySamples() / 2,
                  "the 24 kHz delay should exceed half the 48 kHz figure");
        }
    }

    if (failures == 0) {
        std::printf("nnr_filter_test: all checks passed (delay %.2f ms, "
                    "noise %.1f dB, voice %.1f dB)\n", delayMs, noiseOnly, withVoice);
    }
    return failures == 0 ? 0 : 1;
}
