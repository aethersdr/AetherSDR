// HL2 manual notch, heard through the real Hl2RxDsp.
//
// wdsp_channel_test proves a notch on the bare WdspChannel removes its tone and
// that the mirror axis does not; hl2_notch_seed_test and the rebuild test prove
// the mirror and WDSP agree on how MANY notches exist. Neither says that an
// operator's notch still removes the carrier after the things Hl2RxDsp does to
// it: replay after a rebuild, the global enable, an inactive entry, and the
// NCO moving under an absolute-frequency notch. A bookkeeping bug in any of
// those leaves the counts right and the audio untouched, with no error. This
// measures the audio, offline and sample-counted (blockForOutput).
//
// Stimulus: one tone 1 kHz above the tuned frequency, in wire order, AGC off
// so nothing rescales what the notch removed.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
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
constexpr double kToneHz = 1000.0;
constexpr double kTuneHz = 7'100'000.0;
constexpr double kToneRfHz = kTuneHz + kToneHz;   // USB: above centre is above in RF
constexpr double kNotchWidthHz = 200.0;
constexpr int kSettleSeconds = 2;     // 8192-tap filter fill and mask rebuild
constexpr int kMeasureSeconds = 1;

Hl2RxDsp::Config config()
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = kFs;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.agcMode = 0;               // linear: nothing claws the level back
    cfg.blockForOutput = true;
    return cfg;
}

struct Rig {
    Hl2RxDsp dsp;
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
        dsp.setNotchTuneFrequency(kTuneHz);
    }

    void feedSeconds(int seconds)
    {
        const std::size_t total = static_cast<std::size_t>(seconds) * kFs;
        for (std::size_t off = 0; off < total; off += kEp6BlockSamples) {
            const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, total - off);
            std::vector<std::complex<float>> blk(n);
            for (std::size_t i = 0; i < n; ++i) {
                const double ph = 2.0 * kPi * kToneHz * static_cast<double>(clock++) / kFs;
                blk[i] = std::complex<float>(static_cast<float>(0.1 * std::cos(ph)),
                                             static_cast<float>(-0.1 * std::sin(ph)));
            }
            dsp.processIqBlock(blk);
        }
    }

    // Audio rms after the chain has settled on the current notch state.
    double rms()
    {
        collecting = false;
        feedSeconds(kSettleSeconds);
        sumSquares = 0.0;
        count = 0;
        collecting = true;
        feedSeconds(kMeasureSeconds);
        collecting = false;
        return count > 0 ? std::sqrt(sumSquares / static_cast<double>(count)) : 0.0;
    }
};

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    double baseline = 0.0;
    {
        Rig rig;
        baseline = rig.rms();
    }
    check(baseline > 1.0e-4, "the un-notched tone produces audio");
    const auto ratio = [&](double v) { return v / baseline; };

    // A notch on the tone removes it; on the mirror frequency it does not.
    {
        Rig rig;
        rig.dsp.addNotch(0, kToneRfHz, kNotchWidthHz, true);
        const double notched = rig.rms();
        std::printf("notch on the tone: %.3f of baseline\n", ratio(notched));
        check(rig.dsp.notchCount() == 1 && rig.dsp.wdspNotchCount() == 1,
              "the notch is in the mirror and in WDSP");
        check(notched < baseline * 0.25, "a notch on the tone removes it (below 25 %)");
    }
    {
        Rig rig;
        rig.dsp.addNotch(0, kTuneHz - kToneHz, kNotchWidthHz, true);
        const double mirror = rig.rms();
        std::printf("notch on the mirror: %.3f of baseline\n", ratio(mirror));
        check(mirror > baseline * 0.75,
              "a notch at the mirror frequency leaves the tone alone");
    }

    // The global enable and the per-notch active flag both gate it, and both
    // bring the tone back.
    {
        Rig rig;
        rig.dsp.addNotch(0, kToneRfHz, kNotchWidthHz, true);
        check(rig.rms() < baseline * 0.25, "notch engaged");
        rig.dsp.setNotchesEnabled(false);
        const double off = rig.rms();
        std::printf("notches disabled: %.3f of baseline\n", ratio(off));
        check(off > baseline * 0.75, "disabling all notches restores the tone");
        rig.dsp.setNotchesEnabled(true);
        check(rig.rms() < baseline * 0.25, "re-enabling them removes it again");

        rig.dsp.editNotch(0, kToneRfHz, kNotchWidthHz, false);
        const double inactive = rig.rms();
        std::printf("notch made inactive: %.3f of baseline\n", ratio(inactive));
        check(inactive > baseline * 0.75, "an inactive notch does not remove the tone");
        rig.dsp.editNotch(0, kToneRfHz, kNotchWidthHz, true);
        check(rig.rms() < baseline * 0.25, "reactivating it removes the tone again");

        rig.dsp.removeNotch(0);
        const double removed = rig.rms();
        std::printf("notch removed: %.3f of baseline\n", ratio(removed));
        check(rig.dsp.notchCount() == 0 && rig.dsp.wdspNotchCount() == 0,
              "removal empties the mirror and WDSP");
        check(removed > baseline * 0.75, "a removed notch no longer removes the tone");
    }

    // A rebuild (configure) destroys WDSP's database; the operator's notch has
    // to be replayed into the new channel and still bite.
    {
        Rig rig;
        rig.dsp.addNotch(0, kToneRfHz, kNotchWidthHz, true);
        check(rig.rms() < baseline * 0.25, "notch engaged before the rebuild");
        Hl2RxDsp::Config next = config();
        next.filterHighHz = 2800.0;   // a real change, so the channel is rebuilt
        std::string err;
        const bool ok = rig.dsp.configure(next, &err);
        check(ok, ok ? "reconfigure succeeds" : err.c_str());
        check(rig.dsp.notchCount() == 1 && rig.dsp.wdspNotchCount() == 1,
              "the notch is replayed into the rebuilt channel");
        const double after = rig.rms();
        std::printf("after rebuild: %.3f of baseline\n", ratio(after));
        check(after < baseline * 0.25, "the replayed notch still removes the tone");
    }

    // Notch centres are absolute RF. Retuning moves the tone out from under
    // the notch (the tone stays 1 kHz above the NCO, so it is now at +500 Hz
    // from the notch); tuning back puts it under again.
    {
        Rig rig;
        rig.dsp.addNotch(0, kToneRfHz, kNotchWidthHz, true);
        check(rig.rms() < baseline * 0.25, "notch engaged before the retune");
        rig.dsp.setNotchTuneFrequency(kTuneHz + 500.0);
        const double moved = rig.rms();
        std::printf("NCO moved 500 Hz: %.3f of baseline\n", ratio(moved));
        check(moved > baseline * 0.75,
              "an absolute-frequency notch no longer covers a tone the NCO moved off it");
        rig.dsp.setNotchTuneFrequency(kTuneHz);
        check(rig.rms() < baseline * 0.25, "tuning back puts the notch on the tone again");
    }

    if (g_failures == 0)
        std::fprintf(stdout, "hl2_rxdsp_notch_audio_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
