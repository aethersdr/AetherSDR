// HL2 receive squelch, heard through the real Hl2RxDsp.
//
// wdsp_channel_test pins that the WDSP squelch stage gates audio, per mode
// family, on a bare WdspChannel; hl2_rxdsp_squelch_test pins that the operator's
// request reaches the channel and survives a rebuild, by reading the channel's
// record of what it wrote. Neither listens to Hl2RxDsp's audio. This does, in
// USB: squelch off passes the noise floor, squelch on at level 50 mutes it, a
// signal above the gate opens it, and a rebuild (configure) does not reopen a
// closed gate. Levels are the HL2-measured ones wdsp_channel_test uses: noise
// ~-116 dBFS, signal -92 dBFS, gate at level 50 -105 dBFS.
//
// Offline and sample-counted: blockForOutput, no audio device, no wall clock.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

using namespace AetherSDR::hl2;

namespace {

int g_failures = 0;
void check(bool cond, const char* what)
{
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

constexpr double kPi = 3.14159265358979323846;
constexpr int kFs = 48000;
constexpr float kNoiseRms = 3.3e-6f;       // ~-116 dBFS in the passband
constexpr double kSignalDbfs = -92.0;
constexpr int kSettleSeconds = 2;
constexpr int kMeasureSeconds = 1;

Hl2RxDsp::Config config(double filterHighHz = 2850.0)
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = kFs;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.filterLowHz = 150.0;
    cfg.filterHighHz = filterHighHz;
    cfg.blockForOutput = true;             // default AGC, as on the radio
    return cfg;
}

struct Rig {
    Hl2RxDsp dsp;
    std::mt19937 rng{5678};
    std::normal_distribution<float> gauss{0.0f, 1.0f};
    double sumSquares = 0.0;
    std::size_t count = 0;
    bool collecting = false;
    long clock = 0;

    Rig()
    {
        std::string err;
        const bool ok = dsp.configure(config(), &err);
        check(ok, ok ? "Hl2RxDsp configures" : err.c_str());
        QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                         [this](const std::vector<float>& pcm) {
            if (!collecting)
                return;
            for (float s : pcm) {
                sumSquares += static_cast<double>(s) * s;
                ++count;
            }
        });
    }

    // Noise, plus a USB tone 1 kHz up (wire order) when signalDbfs is finite.
    void feedSeconds(int seconds, double signalDbfs, bool withSignal)
    {
        const double amp = std::pow(10.0, signalDbfs / 20.0);
        const std::size_t total = static_cast<std::size_t>(seconds) * kFs;
        for (std::size_t off = 0; off < total; off += kEp6BlockSamples) {
            const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, total - off);
            std::vector<std::complex<float>> blk(n);
            for (std::size_t i = 0; i < n; ++i) {
                const double ph = 2.0 * kPi * 1000.0 * static_cast<double>(clock++) / kFs;
                double si = 0.0, sq = 0.0;
                if (withSignal) {
                    si = amp * std::cos(ph);
                    sq = -amp * std::sin(ph);
                }
                blk[i] = std::complex<float>(static_cast<float>(si) + kNoiseRms * gauss(rng),
                                             static_cast<float>(sq) + kNoiseRms * gauss(rng));
            }
            dsp.processIqBlock(blk);
        }
    }

    double rms(bool withSignal)
    {
        collecting = false;
        feedSeconds(kSettleSeconds, kSignalDbfs, withSignal);
        sumSquares = 0.0;
        count = 0;
        collecting = true;
        feedSeconds(kMeasureSeconds, kSignalDbfs, withSignal);
        collecting = false;
        return count > 0 ? std::sqrt(sumSquares / static_cast<double>(count)) : -1.0;
    }
};

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    double noiseOpen = 0.0, signalOpen = 0.0;
    {
        Rig rig;
        noiseOpen = rig.rms(false);
        signalOpen = rig.rms(true);
    }
    std::printf("squelch off: noise %.3g, signal %.3g\n", noiseOpen, signalOpen);
    check(noiseOpen > 1.0e-7, "squelch off: the noise floor is audible (the reference)");
    check(signalOpen > 0.0, "squelch off: the signal is audible");

    {
        Rig rig;
        rig.dsp.setSquelch(true, 50, 0.0);
        const double noise = rig.rms(false);
        std::printf("squelch level 50: noise %.3g\n", noise);
        check(noise >= 0.0 && noise < 1.0e-9, "squelch level 50 mutes the noise floor");

        const double signal = rig.rms(true);
        std::printf("squelch level 50: signal %.3g (open %.3g)\n", signal, signalOpen);
        check(signal > 0.5 * signalOpen,
              "a -92 dBFS signal above the -105 dBFS gate opens it");

        // Close again, then rebuild the channel: the gate must stay closed.
        const double closedAgain = rig.rms(false);
        check(closedAgain >= 0.0 && closedAgain < 1.0e-9,
              "with the signal gone the gate closes again");
        std::string err;
        const bool ok = rig.dsp.configure(config(2800.0), &err);
        check(ok, ok ? "reconfigure succeeds" : err.c_str());
        const double afterRebuild = rig.rms(false);
        std::printf("after rebuild: noise %.3g\n", afterRebuild);
        check(afterRebuild >= 0.0 && afterRebuild < 1.0e-9,
              "a rebuild does not reopen the squelch");

        rig.dsp.setSquelch(true, 0, 0.0);
        const double zero = rig.rms(false);
        std::printf("squelch level 0: noise %.3g\n", zero);
        check(zero > 0.5 * noiseOpen, "level 0 is open: the noise comes back");
    }

    if (g_failures == 0)
        std::fprintf(stdout, "hl2_rxdsp_squelch_audio_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
