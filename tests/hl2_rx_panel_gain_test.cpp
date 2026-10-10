// #5942: the HL2 receive chain leaves WDSP at the AGC's level, not 12 dB above it.
// A real Hl2RxDsp, fed a known tone offline. The negative control is the SAME
// built channel config with only rxPanelGain unset (WDSP's 4.0).

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::fprintf(stderr, "  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

static constexpr double kPi = 3.14159265358979323846;
static constexpr int kFs = 48000;
static constexpr std::size_t kBlk = 1024;
static constexpr double kToneHz = 1500.0;   // mid-passband of 150-3000 Hz
static constexpr std::size_t kSettleBlocks = 96;   // ~2 s
static constexpr std::size_t kMeasureBlocks = 48;  // ~1 s

struct Level {
    double rms = 0.0;
    double peak = 0.0;
};

static Level levelOf(const std::vector<float>& x)
{
    Level l;
    double sum = 0.0;
    for (float v : x) {
        sum += static_cast<double>(v) * v;
        l.peak = std::max(l.peak, static_cast<double>(std::fabs(v)));
    }
    l.rms = x.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(x.size()));
    return l;
}

static double db(double ratio) { return 20.0 * std::log10(ratio); }

// The wire convention the other HL2 RX tests use for a tone in the USB passband.
static std::vector<std::complex<float>> wireTone(std::size_t n, std::size_t phase0, double amp)
{
    std::vector<std::complex<float>> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        const double ph = 2.0 * kPi * kToneHz * static_cast<double>(k + phase0) / kFs;
        out[k] = static_cast<float>(amp) * std::complex<float>(static_cast<float>(std::cos(ph)),
                                                               static_cast<float>(-std::sin(ph)));
    }
    return out;
}

static Hl2RxDsp::Config config(WdspChannel::Mode mode, int agcMode)
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = kFs;
    cfg.dspBlockSize = static_cast<int>(kBlk);
    cfg.fftSize = 256;
    cfg.mode = mode;
    cfg.filterLowHz = 150.0;
    cfg.filterHighHz = 3000.0;
    cfg.agcMode = agcMode;
    cfg.blockForOutput = true;
    return cfg;
}

// Left channel of the last kMeasureBlocks, after kSettleBlocks of the same tone.
static Level runHl2(Hl2RxDsp& dsp, double amp, bool lowerSideband = false)
{
    std::vector<float> out;
    bool armed = false;
    const auto conn = QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                                       [&](const std::vector<float>& pcm) {
        if (armed) {
            for (std::size_t i = 0; i < pcm.size(); i += 2) {
                out.push_back(pcm[i]);
            }
        }
    });
    const std::size_t total = (kSettleBlocks + kMeasureBlocks) * kBlk;
    auto tone = wireTone(total, 0, amp);
    if (lowerSideband) {
        for (auto& s : tone) {
            s = std::conj(s);   // the same tone on the other side of the NCO
        }
    }
    for (std::size_t off = 0; off < total; off += kEp6BlockSamples) {
        armed = off >= kSettleBlocks * kBlk;
        const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, total - off);
        dsp.processIqBlock(std::vector<std::complex<float>>(
            tone.begin() + static_cast<std::ptrdiff_t>(off),
            tone.begin() + static_cast<std::ptrdiff_t>(off + n)));
    }
    QObject::disconnect(conn);
    return levelOf(out);
}

// Same tone straight through a WdspChannel built from `wc`.
static Level runChannel(const WdspChannel::Config& wc, double amp)
{
    std::string err;
    auto ch = WdspChannel::create(wc, &err);
    if (!ch) {
        std::fprintf(stderr, "  channel refused: %s\n", err.c_str());
        return {};
    }
    const std::size_t outN = ch->outputBlockSize();
    std::vector<float> i(kBlk), q(kBlk), l(outN), r(outN), out;
    const std::size_t blocks = kSettleBlocks + kMeasureBlocks;
    const auto tone = wireTone(blocks * kBlk, 0, amp);
    for (std::size_t b = 0; b < blocks; ++b) {
        for (std::size_t k = 0; k < kBlk; ++k) {
            i[k] = tone[b * kBlk + k].real();
            q[k] = tone[b * kBlk + k].imag();
        }
        if (ch->processIq(i, q, l, r) == WdspChannel::ProcessResult::Ok && b >= kSettleBlocks) {
            out.insert(out.end(), l.begin(), l.end());
        }
    }
    return levelOf(out);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::fprintf(stderr, "hl2_rx_panel_gain_test (#5942)\n");

    // Leg 1: AGC off, so the chain is a fixed gain and the panel is readable in dB.
    {
        constexpr double amp = 0.01;
        Hl2RxDsp dsp;
        std::string err;
        check(dsp.configure(config(WdspChannel::Mode::Usb, 0), &err), "HL2 USB, AGC off configures");
        const WdspChannel::Config* built = dsp.channelConfig();
        check(built && built->rxPanelGain && *built->rxPanelGain == 1.0,
              "the HL2 channel is opened with an explicit unity panel gain");
        if (!built) {
            return 1;
        }
        const WdspChannel::Config wc = *built;
        const Level hl2 = runHl2(dsp, amp);
        const double gainDb = db(hl2.rms * std::sqrt(2.0) / amp);
        std::fprintf(stderr, "  HL2 chain     : rms %.5f peak %.5f  gain %+.2f dB "
                     "(AGC-off fixed gain %+.2f dB)\n", hl2.rms, hl2.peak, gainDb,
                     wc.agcFixedGainDb);
        check(std::fabs(gainDb - wc.agcFixedGainDb) < 0.5,
              "AGC off, the HL2 chain's gain is the fixed gain alone (+/-0.5 dB)");

        // Positive control: the built config, re-opened directly, reads the same.
        const Level same = runChannel(wc, amp);
        std::fprintf(stderr, "  same config   : rms %.5f  (%+.2f dB vs HL2)\n", same.rms,
                     db(same.rms / hl2.rms));
        check(same.rms > 0.0 && std::fabs(db(same.rms / hl2.rms)) < 0.1,
              "control: the built config re-opened directly matches the HL2 chain (0.1 dB)");

        // Negative control: the identical config with the panel left at WDSP's default.
        WdspChannel::Config dflt = wc;
        dflt.rxPanelGain.reset();
        const Level x4 = runChannel(dflt, amp);
        const double stepDb = db(x4.rms / hl2.rms);
        std::fprintf(stderr, "  panel unset   : rms %.5f  (%+.2f dB vs HL2)\n", x4.rms, stepDb);
        check(std::fabs(stepDb - db(4.0)) < 0.1,
              "negative control: panel left unset reads +12.04 dB (WDSP's 4.0)");
    }

    // Leg 2: the shipped AGC (med, 39 dB ceiling) and a strong station: the output
    // must sit under full scale, where on WDSP's 4.0 it sits at 2.6-3.9.
    for (const auto mode : {WdspChannel::Mode::Usb, WdspChannel::Mode::Lsb}) {
        const bool usb = mode == WdspChannel::Mode::Usb;
        Hl2RxDsp dsp;
        Hl2RxDsp::Config cfg = config(mode, 3);
        if (!usb) {
            cfg.filterLowHz = -3000.0;
            cfg.filterHighHz = -150.0;
        }
        std::string err;
        check(dsp.configure(cfg, &err), "HL2, AGC med configures");
        const Level lv = runHl2(dsp, 0.1, !usb);
        std::fprintf(stderr, "  %s AGC med  : rms %.4f peak %.4f (%+.2f dBFS)\n",
                     usb ? "USB" : "LSB", lv.rms, lv.peak, db(lv.peak));
        check(lv.peak < 1.0, usb ? "USB, AGC med: a strong tone peaks below full scale"
                                 : "LSB, AGC med: a strong tone peaks below full scale");
        check(lv.peak > 0.5, usb ? "USB, AGC med: and is still loud (peak > 0.5)"
                                 : "LSB, AGC med: and is still loud (peak > 0.5)");
    }

    // Leg 3: the setting survives a rebuild to another mode and rate.
    {
        Hl2RxDsp dsp;
        std::string err;
        dsp.configure(config(WdspChannel::Mode::Usb, 3), &err);
        Hl2RxDsp::Config cfg = config(WdspChannel::Mode::Am, 3);
        cfg.inputSampleRateHz = 96000;
        check(dsp.configure(cfg, &err), "HL2 reconfigures to AM at 96 kHz");
        const WdspChannel::Config* built = dsp.channelConfig();
        check(built && built->rxPanelGain && *built->rxPanelGain == 1.0,
              "a rebuilt HL2 channel still carries the unity panel gain");
    }

    // Refusals: the field is receive-only and must be a finite gain.
    {
        std::string err;
        WdspChannel::Config bad;
        bad.rxPanelGain = std::nan("");
        check(!WdspChannel::create(bad, &err), "a NaN panel gain is refused");
        bad.rxPanelGain = -1.0;
        check(!WdspChannel::create(bad, &err), "a negative panel gain is refused");
        WdspChannel::Config tx;
        tx.direction = WdspChannel::Direction::Transmit;
        tx.rxPanelGain = 1.0;
        check(!WdspChannel::create(tx, &err), "a panel gain on a transmit channel is refused");
    }

    std::fprintf(stderr, g_failures ? "FAILED: %d\n" : "OK\n", g_failures);
    return g_failures ? 1 : 0;
}
