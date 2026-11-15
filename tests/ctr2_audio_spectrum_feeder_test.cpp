// Ctr2AudioSpectrumFeeder::barsFromBins with real FFT data: a pure tone must
// light the bar whose band (log, or linear around a CW pitch) contains it, at
// any span. No radio, no audio device; the analyzer is the one the feeder
// uses.

#include "gui/ClientEqFftAnalyzer.h"
#include "gui/Ctr2AudioSpectrumFeeder.h"
#include "models/Ctr2ProxyModel.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;
constexpr double kPi = 3.14159265358979323846;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

int loudestBar(double toneHz, double fs, int low, int span, bool linear = false)
{
    std::vector<float> x(ClientEqFftAnalyzer::kFftSize);
    for (size_t i = 0; i < x.size(); ++i) {
        x[i] = 0.25f * static_cast<float>(std::sin(2.0 * kPi * toneHz * double(i) / fs));
    }
    ClientEqFftAnalyzer fft;
    fft.update(x.data(), static_cast<int>(x.size()));
    const std::vector<float> bars = Ctr2AudioSpectrumFeeder::barsFromBins(
        fft.magnitudesDb(), fs, low, span, fft.coherentGainCorrectionDb(), linear);
    int best = 0;
    for (int i = 1; i < static_cast<int>(bars.size()); ++i) {
        if (bars[i] > bars[best]) {
            best = i;
        }
    }
    return best;
}

void testToneLandsInItsBand()
{
    const int span = 5000;
    const int low = Ctr2ProxyModel::audioSpectrumLowHz(span);
    for (double tone : {150.0, 500.0, 1000.0, 2000.0, 3000.0, 4000.0}) {
        for (double fs : {24000.0, 48000.0}) {
            const int bar = loudestBar(tone, fs, low, span);
            const double lo = Ctr2ProxyModel::audioSpectrumBandEdgeHz(low, span, 32, bar);
            const double hi = Ctr2ProxyModel::audioSpectrumBandEdgeHz(low, span, 32, bar + 1);
            char msg[160];
            std::snprintf(msg, sizeof msg, "%.0f Hz at %.0f Hz sampling lands in bar %d (%.0f-%.0f Hz)",
                          tone, fs, bar, lo, hi);
            check(tone >= lo * 0.97 && tone <= hi * 1.03, msg);
        }
    }
}

// CW: in the linear window around the pitch, a tone lands in the evenly split
// bar that contains it, and a tone at the pitch sits at the middle of the
// screen.
void testToneLandsInItsLinearBand()
{
    int low = 0;
    int high = 0;
    Ctr2ProxyModel::audioSpectrumCwWindow(-200, 200, 600, 24000.0, &low, &high);
    for (double tone : {250.0, 450.0, 600.0, 750.0, 950.0}) {
        for (double fs : {24000.0, 48000.0}) {
            const int bar = loudestBar(tone, fs, low, high, true);
            const double lo = Ctr2ProxyModel::audioSpectrumBandEdgeHz(low, high, 32, bar, true);
            const double hi = Ctr2ProxyModel::audioSpectrumBandEdgeHz(low, high, 32, bar + 1, true);
            char msg[160];
            std::snprintf(msg, sizeof msg,
                          "%.0f Hz at %.0f Hz sampling lands in linear bar %d (%.0f-%.0f Hz)",
                          tone, fs, bar, lo, hi);
            check(tone >= lo - 15.0 && tone <= hi + 15.0, msg);
        }
    }
    const int pitchBar = loudestBar(600.0, 48000.0, low, high, true);
    check(pitchBar == 15 || pitchBar == 16, "a tone at the pitch lights a middle bar");
}

// Too few bins for any band (DC plus one) reads as silence, not a bin
// repeated across every bar.
void testTooFewBinsIsSilence()
{
    for (size_t n : {size_t(0), size_t(1), size_t(2)}) {
        const std::vector<float> bars = Ctr2AudioSpectrumFeeder::barsFromBins(
            std::vector<float>(n, -10.0f), 48000.0, 67, 4000, 0.0f);
        bool silent = bars.size() == size_t(Ctr2AudioSpectrumFeeder::kBars);
        for (float b : bars) {
            silent = silent && std::isinf(b) && b < 0;
        }
        check(silent, "fewer than three bins gives floor bars");
    }
}

} // namespace

int main()
{
    testToneLandsInItsBand();
    testToneLandsInItsLinearBand();
    testTooFewBinsIsSilence();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_audio_spectrum_feeder_test: all checks passed\n");
    return 0;
}
