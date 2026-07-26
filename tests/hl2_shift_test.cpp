// aetherd HL2 — slice-offset (WDSP shift) verification.
//
// The single-DDC backend tunes the slice INSIDE the passband with a WDSP shift
// so the NCO — and therefore the panadapter centre — can stay still. That only
// works if the shift moves the signal by exactly the offset, in the right
// direction. This test pins both.
//
// Offline and synthetic on purpose. The same question measured against
// hpsdrsim was inconclusive because the simulator carries a strong DC offset on
// I (documented in prototypes/hl2/README.md) which the shift translates to the
// offset frequency, where it can be mistaken for the signal. A generated tone
// with no DC component removes that confound, along with the radio, the network
// and the sample-rate plumbing.
//
// Convention under test: a signal sitting `delta` Hz BELOW the tuned slice
// appears at |delta| Hz of audio in LSB. So with the tone fixed relative to the
// NCO and the slice moved UP by `offset`, the audio frequency must RISE by the
// same amount.

#include "core/backends/hl2/Hl2RxDsp.h"

#include <QCoreApplication>

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static constexpr double kPi = 3.14159265358979323846;
static constexpr int kInputRate = 48000;
static constexpr int kAudioRate = 48000;   // keep audio == input so Hz map 1:1
static constexpr int kBlock = 1024;

// Dominant frequency of a real audio buffer, by naive DFT over a coarse grid
// then refined. Small buffers, so clarity beats speed.
static double dominantHz(const std::vector<float>& mono, int rate)
{
    const std::size_t n = mono.size();
    if (n < 64) return -1.0;
    double bestF = -1.0, bestMag = -1.0;
    for (double f = 100.0; f <= rate / 2.0 - 100.0; f += 25.0) {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * kPi * f / rate;
        for (std::size_t k = 0; k < n; ++k) {
            re += mono[k] * std::cos(w * static_cast<double>(k));
            im += mono[k] * std::sin(w * static_cast<double>(k));
        }
        const double mag = re * re + im * im;
        if (mag > bestMag) { bestMag = mag; bestF = f; }
    }
    return bestF;
}

// Feed a complex tone at `toneOffsetHz` relative to the NCO and return the
// dominant audio frequency produced.
static double runTone(Hl2RxDsp& dsp, double toneOffsetHz, double shiftHz)
{
    dsp.setShift(shiftHz);

    std::vector<float> audio;
    QObject::connect(&dsp, &Hl2RxDsp::audioReady,
                     [&audio](const std::vector<float>& stereo) {
        for (std::size_t k = 0; k + 1 < stereo.size(); k += 2)
            audio.push_back(stereo[k]);            // left channel
    });

    // Discard the first blocks: WDSP's pipeline fills, and a shift change slews.
    constexpr int kWarmBlocks = 24;
    constexpr int kMeasureBlocks = 24;
    double phase = 0.0;
    const double dp = 2.0 * kPi * toneOffsetHz / kInputRate;
    for (int b = 0; b < kWarmBlocks + kMeasureBlocks; ++b) {
        std::vector<std::complex<float>> iq(kBlock);
        for (int k = 0; k < kBlock; ++k) {
            iq[static_cast<std::size_t>(k)] = {
                static_cast<float>(0.25 * std::cos(phase)),
                static_cast<float>(0.25 * std::sin(phase))
            };
            phase += dp;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        }
        if (b == kWarmBlocks) audio.clear();
        dsp.processIqBlock(iq);
    }
    QObject::disconnect(&dsp, &Hl2RxDsp::audioReady, nullptr, nullptr);
    return dominantHz(audio, kAudioRate);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2RxDsp dsp;
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kInputRate;
    cfg.audioSampleRateHz = kAudioRate;
    cfg.dspBlockSize = kBlock;
    cfg.fftSize = 256;
    cfg.mode = WdspChannel::Mode::Lsb;
    cfg.filterLowHz = -9000.0;          // wide, so a shifted tone stays in band
    cfg.filterHighHz = -100.0;
    cfg.agcMode = 0;                    // AGC off: linear, so nothing rescales
    cfg.maximumAgcGainDb = 40.0;
    cfg.blockForOutput = true;          // deterministic for an offline burst feed
    std::string err;
    check(dsp.configure(cfg, &err), err.empty() ? "Hl2RxDsp configures" : err.c_str());
    if (g_failures) return 1;

    // Tone 800 Hz BELOW the NCO. With the slice on the NCO (shift 0) LSB should
    // put it at 800 Hz of audio.
    const double base = runTone(dsp, -800.0, 0.0);
    std::fprintf(stderr, "offset     0 Hz -> audio %.0f Hz (expect ~800)\n", base);
    check(std::fabs(base - 800.0) < 120.0, "tone lands at 800 Hz with no shift");

    // Move the slice 2 kHz ABOVE the NCO. The tone is now 2800 Hz below the
    // slice, so the audio frequency must RISE to ~2800.
    const double up2k = runTone(dsp, -800.0, 2000.0);
    std::fprintf(stderr, "offset +2000 Hz -> audio %.0f Hz (expect ~2800)\n", up2k);
    check(std::fabs(up2k - 2800.0) < 200.0, "slice +2 kHz moves the tone to 2800 Hz");

    // And 4 kHz above.
    const double up4k = runTone(dsp, -800.0, 4000.0);
    std::fprintf(stderr, "offset +4000 Hz -> audio %.0f Hz (expect ~4800)\n", up4k);
    check(std::fabs(up4k - 4800.0) < 250.0, "slice +4 kHz moves the tone to 4800 Hz");

    // Returning the slice to the NCO must return the tone to where it started —
    // the stage is not accumulating.
    const double back = runTone(dsp, -800.0, 0.0);
    std::fprintf(stderr, "offset     0 Hz -> audio %.0f Hz (expect ~800 again)\n", back);
    check(std::fabs(back - 800.0) < 120.0, "shift back to 0 restores the tone");

    if (g_failures == 0) std::fprintf(stderr, "hl2_shift_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
