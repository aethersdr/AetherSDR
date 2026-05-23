// Unit tests for fractional-octave smoothing on ClientEqCurveWidget.
//
// The smoothing function is exposed as a free static method so this
// harness doesn't need a QApplication / live widget.

#include "gui/ClientEqCurveWidget.h"

#include <cmath>
#include <cstdio>
#include <vector>

using MasterSDR::ClientEqCurveWidget;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

bool nearlyEq(float a, float b, float tol = 0.01f)
{
    return std::fabs(a - b) < tol;
}

// 1025-bin (kFftSize/2 + 1 for kFftSize=2048) is the production size.
constexpr int kBins = 1025;
constexpr double kSampleRate = 24000.0;

void testOffReturnsInputUnchanged()
{
    std::vector<float> input(kBins);
    for (int i = 0; i < kBins; ++i)
        input[i] = -50.0f + (i % 10) * 1.5f;  // arbitrary spiky shape

    auto out = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        input, kSampleRate, 96);
    bool match = (out.size() == input.size());
    for (size_t i = 0; i < out.size() && match; ++i) {
        if (!nearlyEq(out[i], input[i], 0.001f)) match = false;
    }
    report("Off (1/96) returns input unchanged", match);

    // Out-of-range fraction also no-op (defensive)
    auto out2 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(input, kSampleRate, 0);
    report("fraction=0 returns input unchanged",
           out2.size() == input.size() && nearlyEq(out2[100], input[100]));
}

void testFlatInputStaysFlat()
{
    std::vector<float> flat(kBins, -30.0f);
    for (int frac : {3, 6, 12, 24}) {
        auto out = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
            flat, kSampleRate, frac);
        bool ok = (out.size() == flat.size());
        for (int i = 1; i < kBins && ok; ++i) {
            if (!nearlyEq(out[i], -30.0f, 0.05f)) ok = false;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "flat input stays flat at 1/%d", frac);
        report(name, ok);
    }
}

void testImpulseSpreads()
{
    // Single-bin spike at 1 kHz (~bin 85 at fs=24k, fft=2048 → 11.7 Hz/bin)
    constexpr float floor = -80.0f;
    std::vector<float> impulse(kBins, floor);
    const int spikeBin = 85;
    impulse[spikeBin] = 0.0f;

    auto raw = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        impulse, kSampleRate, 96);
    auto smoothed = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        impulse, kSampleRate, 6);

    // Raw has the spike isolated.
    report("impulse: raw has spike at bin",
           raw[spikeBin] > -1.0f && raw[spikeBin - 5] < -70.0f);

    // Smoothed has spike spread to neighbors (raised them above the floor).
    bool spread = (smoothed[spikeBin - 5] > floor + 5.0f
                && smoothed[spikeBin + 5] > floor + 5.0f);
    report("impulse: 1/6 spreads spike to neighbors", spread);

    // Smoothed peak is lower than raw peak (energy distributed over window).
    report("impulse: smoothed peak < raw peak",
           smoothed[spikeBin] < raw[spikeBin]);
}

void testTighterFractionMeansLessSmoothing()
{
    // Construct stairs: every 10th bin is +20 dB, others -40 dB.
    constexpr float lo = -40.0f;
    constexpr float hi = -20.0f;
    std::vector<float> stairs(kBins, lo);
    for (int i = 50; i < kBins; i += 10) stairs[i] = hi;

    auto s3  = ClientEqCurveWidget::applyFractionalOctaveSmoothing(stairs, kSampleRate, 3);
    auto s12 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(stairs, kSampleRate, 12);
    auto s24 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(stairs, kSampleRate, 24);

    // Compute spread (max - min) at high freq where smoothing diff is biggest.
    auto spread = [](const std::vector<float>& v) {
        float mn = v[400], mx = v[400];
        for (int i = 400; i < 600; ++i) {
            mn = std::min(mn, v[i]);
            mx = std::max(mx, v[i]);
        }
        return mx - mn;
    };

    // Heavier smoothing (1/3) flattens the stairs more than light (1/24).
    report("1/3 spread < 1/12 spread (more smoothing)", spread(s3) < spread(s12));
    report("1/12 spread < 1/24 spread (more smoothing)", spread(s12) < spread(s24));
}

void testWindowScalesWithFrequency()
{
    // At higher frequencies a 1/N-octave window covers more bins than at
    // lower frequencies (because bin width is constant but octave width
    // grows linearly with frequency).  Probe with a single-bin spike at
    // two different frequencies and check the smoothed peak is more
    // spread (lower / wider) at the higher frequency.
    constexpr float floor = -80.0f;
    std::vector<float> low(kBins, floor);
    std::vector<float> high(kBins, floor);
    low[20]  = 0.0f;   // ~234 Hz
    high[400] = 0.0f;  // ~4.7 kHz

    auto loSmooth  = ClientEqCurveWidget::applyFractionalOctaveSmoothing(low,  kSampleRate, 6);
    auto hiSmooth  = ClientEqCurveWidget::applyFractionalOctaveSmoothing(high, kSampleRate, 6);

    // Smoothed peak at higher frequency should be lower (more bins to
    // average over → energy spread thinner).
    report("smoothing window scales with freq (high peak < low peak)",
           hiSmooth[400] < loSmooth[20]);
}

void testEmptyInputHandled()
{
    std::vector<float> empty;
    auto out = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        empty, kSampleRate, 6);
    report("empty input doesn't crash", out.empty());
}

void testIdempotentForOff()
{
    std::vector<float> input(kBins);
    for (int i = 0; i < kBins; ++i)
        input[i] = -40.0f + 0.05f * static_cast<float>(i);

    auto pass1 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        input, kSampleRate, 96);
    auto pass2 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        pass1, kSampleRate, 96);

    bool same = (pass1.size() == pass2.size());
    for (size_t i = 0; i < pass1.size() && same; ++i) {
        if (!nearlyEq(pass1[i], pass2[i], 0.001f)) same = false;
    }
    report("Off is idempotent", same);
}

void testDbFloorPreserved()
{
    // Silent input at the floor stays close to the floor.
    constexpr float floor = -100.0f;
    std::vector<float> silent(kBins, floor);
    auto out = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        silent, kSampleRate, 6);

    bool ok = (out.size() == silent.size());
    for (int i = 1; i < kBins && ok; ++i) {
        // Smoothed output mustn't drop to -inf (algorithm has an
        // epsilon floor) or wander significantly above the input floor.
        if (!std::isfinite(out[i]) || out[i] > floor + 0.5f) ok = false;
    }
    report("dB floor preserved on silent input", ok);
}

}  // namespace

int main()
{
    testOffReturnsInputUnchanged();
    testFlatInputStaysFlat();
    testImpulseSpreads();
    testTighterFractionMeansLessSmoothing();
    testWindowScalesWithFrequency();
    testEmptyInputHandled();
    testIdempotentForOff();
    testDbFloorPreserved();

    if (g_failed == 0) {
        std::printf("\nAll EQ smoothing tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) FAILED.\n", g_failed);
    return 1;
}
