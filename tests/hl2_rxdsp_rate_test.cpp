// The RX DSP must demodulate at EVERY IQ rate the HL2 can run, in the exact
// configuration Hl2Backend uses in production.
//
// hl2_rxdsp_test only ever ran 48 kHz in / 48 kHz audio out. Production runs
// 24 kHz audio (AudioEngine's native rate) and, since the panadapter span became
// operator-controllable, any of 48/96/192/384 kHz in. Nothing exercised that
// grid, so a rate at which the demodulator goes silent was invisible: the
// channel opens without error (validateConfig only checks integral ratios) and
// the panadapter keeps working because it does its own FFT and never touches
// WdspChannel.
//
// The failure this pins is silent by construction. Assert on AUDIO, at each
// rate, against the same tone.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
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

// Demodulate a tone 1 kHz above centre at `rateHz` and return the peak |sample|
// of the audio that came out.
static float demodPeakAt(int rateHz, std::size_t* outBlockSamples,
                         int* audioBlocks, std::string* err)
{
    Hl2RxDsp dsp;
    Hl2RxDsp::Config cfg;
    // EXACTLY what Hl2Backend::connectRadio and setPanBandwidth build.
    cfg.inputSampleRateHz = rateHz;
    cfg.audioSampleRateHz = 24000;    // AudioEngine's native RX rate
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 1024;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.filterLowHz = 150.0;
    cfg.filterHighHz = 3000.0;
    cfg.blockForOutput = true;        // deterministic for an offline burst feed

    if (!dsp.configure(cfg, err))
        return -1.0f;

    float peak = 0.0f;
    int blocks = 0;
    std::size_t blockSamples = 0;
    QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                     [&](const std::vector<float>& pcm) {
        ++blocks;
        blockSamples = pcm.size();
        for (const float v : pcm)
            peak = std::max(peak, std::abs(v));
    });

    // 0.5 s of a tone 1 kHz above centre, delivered as 126-sample EP6-shaped
    // blocks — IN WIRE ORDER, note the NEGATIVE sine. The HPSDR wire is the
    // conjugate of the analytic convention, so a signal above centre arrives as
    // exp(-j.2.pi.f.t), and the demodulator is fed the raw wire (HERMES §16).
    // The textbook exp(+j...) is synthetic IQ no HL2 ever sends, and against
    // USB [150,3000] it is out of passband: this same generator produced
    // audible audio only while the chain inverted every sideband.
    const double f = 1000.0;
    const int total = rateHz / 2;
    std::vector<std::complex<float>> blk;
    blk.reserve(kEp6BlockSamples);
    for (int n = 0; n < total; ++n) {
        const double ph = 2.0 * kPi * f * n / rateHz;
        blk.emplace_back(0.3f * static_cast<float>(std::cos(ph)),
                         0.3f * static_cast<float>(-std::sin(ph)));
        if (static_cast<int>(blk.size()) == kEp6BlockSamples) {
            dsp.processIqBlock(blk);
            blk.clear();
        }
    }
    if (!blk.empty())
        dsp.processIqBlock(blk);

    if (outBlockSamples) *outBlockSamples = blockSamples;
    if (audioBlocks) *audioBlocks = blocks;
    return peak;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The four rates the gateware can run — the same list Hl2Backend advertises
    // and snaps zoom requests to.
    struct Row { int rateHz; float peak; std::size_t blockSamples; int blocks; };
    Row rows[] = {{48000, 0, 0, 0}, {96000, 0, 0, 0},
                  {192000, 0, 0, 0}, {384000, 0, 0, 0}};

    for (auto& r : rows) {
        std::string err;
        r.peak = demodPeakAt(r.rateHz, &r.blockSamples, &r.blocks, &err);
        if (r.peak < 0.0f) {
            std::fprintf(stderr, "FAIL: %d Hz did not configure: %s\n",
                         r.rateHz, err.c_str());
            ++g_failures;
            continue;
        }
        // outputBlockSize = dspBlockSize * 24000 / rate, interleaved stereo.
        const std::size_t expect =
            static_cast<std::size_t>(1024) * 24000 / static_cast<std::size_t>(r.rateHz) * 2;
        std::fprintf(stderr,
                     "%6d Hz in -> audio peak %.5f, %d blocks of %zu samples "
                     "(expect %zu)\n",
                     r.rateHz, static_cast<double>(r.peak), r.blocks,
                     r.blockSamples, expect);
        check(r.blockSamples == expect, "audio block is the rate-scaled size");
        check(r.blocks > 0, "audio blocks were produced at this rate");
        // THE assertion. A silent demodulator at a rate the operator can select
        // by zooming is the bug; everything above is diagnosis for when it trips.
        check(r.peak > 0.01f, "demodulator produces audible audio at this rate");
    }

    // And the levels must AGREE across rates. The same tone at the same
    // amplitude has to come out at the same level whatever the DDC is doing —
    // otherwise zooming would change how loud the radio is.
    float lo = 1e9f, hi = 0.0f;
    for (const auto& r : rows) {
        if (r.peak <= 0.0f) continue;
        lo = std::min(lo, r.peak);
        hi = std::max(hi, r.peak);
    }
    if (hi > 0.0f && lo < 1e9f) {
        const double spreadDb = 20.0 * std::log10(hi / lo);
        std::fprintf(stderr, "level spread across rates: %.1f dB\n", spreadDb);
        check(spreadDb < 6.0, "audio level is consistent across every IQ rate");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_rxdsp_rate_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
