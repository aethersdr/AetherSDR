// aetherd HL2 -- #5498: how long after an unkey does the band come back, and
// is any of that time LOST audio or only the receive chain's own latency?
//
// WHAT THIS PINS
//
// The "~0.17 s of receive lost after every unkey" #5498 measured is timed from
// the demodulator unmute. This test puts a signal ON THE AIR THROUGHOUT -- before
// the key, during it, after it -- and times when it reappears after the unmute.
// It compares that against the chain's plain latency: the same chain, never
// muted, meeting the same signal for the first time. The first property is:
//
//   1. THE RETURN IS NO SLOWER THAN THE LATENCY. Nothing after the unmute is
//      dropped; the band comes back on the same delay every receive sample
//      always has. What #5498 timed as a dropout is that delay, seen from the
//      unmute.
//
// So the only lever on the return is the latency itself, and the latency is
// dominated by the RX bandpass: a linear-phase FIR of kRxFilterTaps delays by
// half its length, ~85 ms. Hl2RxDsp runs it at MINIMUM phase outside CW
// (Hl2RxDsp::rxMinimumPhaseFor, a maintainer ruling on #5498), which is the
// second property and the fix:
//
//   2. OUTSIDE CW THE RETURN IS SHORT -- measured ~44 ms with the AGC off,
//      against ~128 ms on the same chain at linear phase.
//   3. CW KEEPS LINEAR PHASE -- the ruling, because minimum phase costs a
//      narrow CW filter overshoot and ringing (#5578).
//   4. THE PHASE FOLLOWS THE MODE on all three roads a mode reaches the
//      channel: the build, a live setMode(), and a mode change that lands
//      while a background rebuild is in flight and is re-applied at install.
//
// Socket-free, no radio: a real Hl2RxDsp, fed EP6-shaped blocks on this thread,
// AGC off so the timing is the filter's and not the AGC's recovery.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
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
static constexpr int kFs = 48000;            // input rate == audio rate == DSP rate
static constexpr std::size_t kBlk = 1024;
static constexpr double kToneHz = 1500.0;    // mid-passband for the 150-3000 Hz filter
static constexpr double kToneAmp = 0.02;     // clear of the rail with the AGC off

// Hann-windowed one-bin DFT at kToneHz, 10 ms window stepped 1 ms: a matched
// filter, so "the tone is back" cannot be confused with "something is back".
static std::vector<double> toneProfile(const std::vector<float>& x)
{
    const int win = kFs / 100;
    const int hop = kFs / 1000;
    std::vector<double> wc(static_cast<std::size_t>(win));
    std::vector<double> ws(static_cast<std::size_t>(win));
    double wsum = 0.0;
    for (int n = 0; n < win; ++n) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (win - 1)));
        wsum += w;
        wc[static_cast<std::size_t>(n)] = w * std::cos(2.0 * kPi * kToneHz * n / kFs);
        ws[static_cast<std::size_t>(n)] = w * std::sin(2.0 * kPi * kToneHz * n / kFs);
    }
    std::vector<double> out;
    for (std::size_t off = 0; off + static_cast<std::size_t>(win) <= x.size();
         off += static_cast<std::size_t>(hop)) {
        double re = 0.0;
        double im = 0.0;
        for (int n = 0; n < win; ++n) {
            re += x[off + static_cast<std::size_t>(n)] * wc[static_cast<std::size_t>(n)];
            im -= x[off + static_cast<std::size_t>(n)] * ws[static_cast<std::size_t>(n)];
        }
        out.push_back(2.0 * std::hypot(re, im) / wsum);
    }
    return out;
}

// Milliseconds from the start of `x` until the tone first reaches -6 dB of its
// own settled level (taken well after it has settled). -1 if it never does.
static double returnMs(const std::vector<float>& x)
{
    const auto p = toneProfile(x);
    if (p.size() < 400) {
        return -1.0;
    }
    const double settled = p[p.size() - 200];
    if (settled <= 0.0) {
        return -1.0;
    }
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p[i] >= settled * 0.5) {                  // -6 dB
            return (static_cast<double>(i) * (kFs / 1000) + kFs / 200.0) * 1000.0 / kFs;
        }
    }
    return -1.0;
}

// A steady tone above centre in WIRE order (the HPSDR wire is the conjugate of
// the analytic convention), continuous in phase across calls via `phase0`.
static std::vector<std::complex<float>> wireTone(std::size_t n, std::size_t phase0)
{
    std::vector<std::complex<float>> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        const double ph = 2.0 * kPi * kToneHz * static_cast<double>(k + phase0) / kFs;
        out[k] = static_cast<float>(kToneAmp)
                 * std::complex<float>(static_cast<float>(std::cos(ph)),
                                       static_cast<float>(-std::sin(ph)));
    }
    return out;
}

// EP6-shaped blocks, as MetisClient delivers them.
static void feed(Hl2RxDsp& dsp, const std::vector<std::complex<float>>& s)
{
    for (std::size_t off = 0; off < s.size(); off += kEp6BlockSamples) {
        const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, s.size() - off);
        dsp.processIqBlock(std::vector<std::complex<float>>(
            s.begin() + static_cast<std::ptrdiff_t>(off),
            s.begin() + static_cast<std::ptrdiff_t>(off + n)));
    }
}

static Hl2RxDsp::Config config(WdspChannel::Mode mode)
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = kFs;
    cfg.dspBlockSize = static_cast<int>(kBlk);
    cfg.fftSize = 256;
    cfg.mode = mode;
    // The SAME passband in every leg, CW included, so the only thing that
    // differs between them is the phase the mode selects.
    cfg.filterLowHz = 150.0;
    cfg.filterHighHz = 3000.0;
    cfg.agcMode = 0;               // AGC off: time the filter, not the AGC
    cfg.maximumAgcGainDb = 39.0;
    cfg.blockForOutput = true;
    return cfg;
}

// Collects the left channel of everything the chain emits after `armed` is set.
struct Capture {
    std::vector<float> after;
    bool armed = false;
    void attach(Hl2RxDsp& dsp)
    {
        QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                         [this](const std::vector<float>& pcm) {
            if (!armed) {
                return;
            }
            for (std::size_t i = 0; i < pcm.size(); i += 2) {
                after.push_back(pcm[i]);
            }
        });
    }
};

// The #5498 shape: the tone is on the air throughout, the receiver is muted for
// ~512 ms (an over), and the return is timed from the unmute.
static double unmuteReturnMs(Hl2RxDsp& dsp)
{
    Capture cap;
    cap.attach(dsp);
    std::size_t ph = 0;
    feed(dsp, wireTone(144 * kBlk, ph));   // ~3 s: the chain settled on the tone
    ph += 144 * kBlk;
    dsp.setAudioMuted(true);
    feed(dsp, wireTone(24 * kBlk, ph));    // the signal stays on the air
    ph += 24 * kBlk;
    dsp.setAudioMuted(false);
    cap.armed = true;
    feed(dsp, wireTone(96 * kBlk, ph));
    return returnMs(cap.after);
}

// The chain's plain latency: never muted, meeting the tone for the first time
// after ~1 s of silence.
static double freshOnsetMs(Hl2RxDsp& dsp)
{
    Capture cap;
    cap.attach(dsp);
    feed(dsp, std::vector<std::complex<float>>(48 * kBlk));
    cap.armed = true;
    feed(dsp, wireTone(144 * kBlk, 0));
    return returnMs(cap.after);
}

static bool configure(Hl2RxDsp& dsp, WdspChannel::Mode mode)
{
    std::string err;
    const bool ok = dsp.configure(config(mode), &err);
    if (!ok) {
        std::fprintf(stderr, "  configure failed: %s\n", err.c_str());
    }
    return ok;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- the ruling, as a table -------------------------------------------
    using M = WdspChannel::Mode;
    check(!Hl2RxDsp::rxMinimumPhaseFor(M::Cwu) && !Hl2RxDsp::rxMinimumPhaseFor(M::Cwl),
          "ruling: CW (both sidebands) keeps LINEAR phase");
    bool othersMp = true;
    for (M m : {M::Lsb, M::Usb, M::Dsb, M::Fm, M::Am, M::Digu, M::Spec, M::Digl,
                M::Sam, M::Drm, M::Wbfm}) {
        othersMp = othersMp && Hl2RxDsp::rxMinimumPhaseFor(m);
    }
    check(othersMp, "ruling: every other mode runs MINIMUM phase");

    // ---- 1 + 2: USB -------------------------------------------------------
    double usbReturn = -1.0;
    double usbFresh = -1.0;
    {
        Hl2RxDsp dsp;
        check(configure(dsp, M::Usb), "USB: configures");
        usbReturn = unmuteReturnMs(dsp);
    }
    {
        Hl2RxDsp dsp;
        check(configure(dsp, M::Usb), "USB (fresh onset): configures");
        usbFresh = freshOnsetMs(dsp);
    }
    std::printf("      MEASURED: USB return after unmute %.1f ms; fresh-onset latency %.1f ms\n",
                usbReturn, usbFresh);
    check(usbReturn > 0.0 && usbFresh > 0.0, "USB: the tone comes back and is measurable");
    check(usbReturn <= usbFresh + 5.0,
          "USB: the return is NO SLOWER than the chain's own latency -- nothing "
          "after the unmute is lost (#5498)");

    // ---- 3: CW, same passband ---------------------------------------------
    double cwReturn = -1.0;
    double cwFresh = -1.0;
    {
        Hl2RxDsp dsp;
        check(configure(dsp, M::Cwu), "CWU: configures");
        cwReturn = unmuteReturnMs(dsp);
    }
    {
        Hl2RxDsp dsp;
        check(configure(dsp, M::Cwu), "CWU (fresh onset): configures");
        cwFresh = freshOnsetMs(dsp);
    }
    std::printf("      MEASURED: CWU return after unmute %.1f ms; fresh-onset latency %.1f ms\n",
                cwReturn, cwFresh);
    check(cwReturn > 0.0, "CWU: the tone comes back and is measurable");
    check(cwReturn <= cwFresh + 5.0,
          "CWU: the return is no slower than the chain's own latency either");
    // Linear phase at 8192 taps is 4096 samples (85.3 ms) of group delay that
    // minimum phase removes; allow generous room either side of it.
    check(usbReturn > 0.0 && cwReturn - usbReturn > 60.0,
          "the fix: outside CW the band comes back >60 ms sooner than on the "
          "linear-phase chain CW keeps");
    check(usbReturn > 0.0 && usbReturn < 80.0,
          "the fix: the USB return is under 80 ms (was ~128 ms at linear phase)");

    // ---- 4a: a live setMode() moves the phase -----------------------------
    {
        Hl2RxDsp dsp;
        check(configure(dsp, M::Usb), "live USB -> CWU: configures");
        dsp.setMode(M::Cwu);
        const double r = unmuteReturnMs(dsp);
        std::printf("      MEASURED: USB then setMode(CWU): return %.1f ms\n", r);
        check(r > 0.0 && std::fabs(r - cwReturn) < 15.0,
              "live setMode(CWU) puts the chain back on LINEAR phase");
    }
    {
        Hl2RxDsp dsp;
        check(configure(dsp, M::Cwu), "live CWU -> USB: configures");
        dsp.setMode(M::Usb);
        const double r = unmuteReturnMs(dsp);
        std::printf("      MEASURED: CWU then setMode(USB): return %.1f ms\n", r);
        check(r > 0.0 && std::fabs(r - usbReturn) < 15.0,
              "live setMode(USB) puts the chain on MINIMUM phase");
    }

    // ---- 4b: a mode change during a background rebuild --------------------
    // The chain is built for the Config it was handed (USB, so minimum phase);
    // the operator's CWU lands in the object's own Config while the build is
    // in flight and must win at the install.
    {
        Hl2RxDsp dsp;
        check(configure(dsp, M::Usb), "rebuild: configures");
        const Hl2RxDsp::Config usbCfg = config(M::Usb);
        dsp.beginRebuild(usbCfg);
        dsp.setMode(M::Cwu);   // deferred: a rebuild is in flight
        check(dsp.installRebuiltChannel(Hl2RxDsp::buildChannel(usbCfg, false, 0)),
              "rebuild: the rebuilt chain installs");
        const double r = unmuteReturnMs(dsp);
        std::printf("      MEASURED: rebuilt for USB, CWU set mid-build: return %.1f ms\n", r);
        check(r > 0.0 && std::fabs(r - cwReturn) < 15.0,
              "rebuild: the mode set mid-build decides the phase at install (LINEAR for CW)");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "hl2_rxdsp_unmute_return_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
