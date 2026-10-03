// aetherd HL2 -- #5578 / #5678 row 4.1: the RX bandpass opens SHORT in CW and
// buys the long filter only when something needs its resolution.
//
// WHAT THIS PINS
//
// #5954 took the RX bandpass to minimum phase outside CW and left CW on a
// linear-phase FIR of Hl2RxDsp::kRxFilterTaps (8192), which delays everything
// by (taps-1)/2 = 4095.5 samples, 85.3 ms at the 48 kHz DSP rate. The 8192 is
// bought for two things: WDSP's notch-width floor, 1600/(taps/256) Hz (50 Hz
// at 8192, 200 Hz at 2048), and -- this test establishes -- the skirt. #5678
// row 4.1 proposed 2048; measured here, 2048 puts a signal 50 Hz outside the
// filter edge at -33 dB instead of -114, so the short length is 4096. Hl2RxDsp::rxFilterTapsFor() now picks the
// length, and this test is its evidence:
//
//   1. MEASURED, NOT RETYPED: magnitude response of every CW preset width at
//      2048 / 4096 / 8192 taps through a real WdspChannel (impulse response,
//      AGC off), and CW onset latency at each length (on8st's #5578 method:
//      noise-primed so WDSP's one-shot up-slew is spent, then half the settled
//      amplitude). The policy's width thresholds are asserted against these
//      measurements, so moving a threshold below what its length realises
//      goes red here.
//   2. THE POLICY: a pure function, table-tested -- CW without a notch runs
//      4096 when the passband is >= 100 Hz; any notch, a narrower passband,
//      and every mode outside CW runs 8192.
//   3. THE WIRING: the length follows mode, passband and notch count on the
//      live chain and across a rebuild, and it is raised BEFORE the first
//      notch lands (so its 50 Hz width is not silently widened to 200 Hz).
//   4. THE SWITCH: raising the length mid-stream produces no NaN, no spike,
//      a bounded refill gap, and the same settled level; a notch placed
//      across the switch is as deep as one placed on a chain opened long.
//
// Socket-free, no radio.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::hl2;

namespace {

int g_failures = 0;
void check(bool cond, const char* what)
{
    std::fprintf(stderr, "  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

constexpr double kPi = 3.14159265358979323846;
constexpr int kFs = 48000;
constexpr std::size_t kBlk = 1024;
constexpr double kPitchHz = 700.0;
constexpr double kDb = 20.0;

double db(double x) { return kDb * std::log10(std::max(x, 1e-15)); }

// ---- 1. direct WdspChannel measurements ---------------------------------

WdspChannel::Config channelConfig(WdspChannel::Mode mode, double lo, double hi,
                                  int taps, bool minimumPhase)
{
    WdspChannel::Config c;
    c.inputBlockSize = kBlk;
    c.dspBlockSize = kBlk;        // Hl2RxDsp's own block
    c.inputSampleRate = kFs;
    c.dspSampleRate = kFs;
    c.outputSampleRate = kFs;
    c.mode = mode;
    c.filterLowHz = lo;
    c.filterHighHz = hi;
    c.agcMode = 0;                // AGC off: measure the filter, not the AGC
    c.agcFixedGainDb = 0.0;
    c.blockForOutput = true;
    c.filterTaps = taps;
    c.minimumPhase = minimumPhase;
    return c;
}

// Pushes a complex input through the channel and returns the left output.
bool run(WdspChannel& ch, const std::vector<std::complex<float>>& in, std::vector<float>& out)
{
    std::vector<float> i(kBlk), q(kBlk), l(ch.outputBlockSize()), r(ch.outputBlockSize());
    for (std::size_t off = 0; off + kBlk <= in.size(); off += kBlk) {
        for (std::size_t k = 0; k < kBlk; ++k) {
            i[k] = in[off + k].real();
            q[k] = in[off + k].imag();
        }
        if (ch.processIq(i, q, l, r) != WdspChannel::ProcessResult::Ok)
            return false;
        out.insert(out.end(), l.begin(), l.end());
    }
    return true;
}

// Low-level noise, so WDSP's one-shot up-slew (iobuffs.c upslew2, which waits
// for the first non-zero sample) fires and finishes BEFORE anything is timed --
// the correction on8st posted on #5578 after a silent settle put the ramp
// inside the window and read 22.6 ms high.
std::vector<std::complex<float>> primer(std::size_t n)
{
    std::mt19937 rng(5578);
    std::normal_distribution<float> g(0.0f, 1e-4f);
    std::vector<std::complex<float>> v(n);
    for (auto& s : v)
        s = {g(rng), g(rng)};
    return v;
}

// |H(f)| of a real impulse response on a 1 Hz grid, 0..maxHz.
std::vector<double> magnitude(const std::vector<float>& h, int maxHz)
{
    std::vector<double> m(static_cast<std::size_t>(maxHz) + 1);
    for (int f = 0; f <= maxHz; ++f) {
        const std::complex<double> w = std::polar(1.0, -2.0 * kPi * f / kFs);
        std::complex<double> rot = 1.0;
        std::complex<double> acc = 0.0;
        for (float s : h) {
            acc += static_cast<double>(s) * rot;
            rot *= w;
        }
        m[static_cast<std::size_t>(f)] = std::abs(acc);
    }
    return m;
}

// Impulse response of the whole RXA chain, CWU at `taps`, passband lo..hi Hz,
// from the sample the impulse went in.
std::vector<float> cwImpulse(double lo, double hi, int taps)
{
    std::string err;
    auto ch = WdspChannel::create(channelConfig(WdspChannel::Mode::Cwu, lo, hi, taps, false), &err);
    if (!ch)
        return {};
    std::vector<float> out;
    auto in = primer(16 * kBlk);                            // spend the up-slew
    in.resize(in.size() + 32 * kBlk);                       // ~0.7 s of zeros: flush it
    const std::size_t at = in.size();
    in.resize(at + 24 * kBlk);
    in[at] = {1.0f, 0.0f};                                  // the impulse
    if (!run(*ch, in, out))
        return {};
    // Everything the impulse produced: pipeline + taps fit well inside 24 blocks.
    return std::vector<float>(out.begin() + static_cast<std::ptrdiff_t>(at), out.end());
}

std::vector<double> cwResponse(double lo, double hi, int taps)
{
    const auto h = cwImpulse(lo, hi, taps);
    return h.empty() ? std::vector<double>{} : magnitude(h, 3000);
}

// Centre of energy of an impulse response, in samples. For a linear-phase
// (symmetric) FIR it is EXACTLY the group delay plus the chain's fixed
// pipeline, whatever the skirt looks like -- unlike a half-amplitude onset,
// which adds each length's own step-response rise (review of #5981).
double energyCentre(const std::vector<float>& h)
{
    double e = 0.0, m = 0.0;
    for (std::size_t k = 0; k < h.size(); ++k) {
        const double p = static_cast<double>(h[k]) * h[k];
        e += p;
        m += p * static_cast<double>(k);
    }
    return e > 0.0 ? m / e : -1.0;
}

struct Shape {
    double centreDb = 0.0;    // gain at the passband centre, re the reference
    double bw6 = 0.0;         // -6 dB bandwidth, Hz
    double bw60 = 0.0;        // -60 dB bandwidth, Hz
    double at50 = 0.0;        // worst response 50 Hz or more outside the edges, dB
    double at100 = 0.0;       // ... 100 Hz or more outside
};

Shape shapeOf(const std::vector<double>& m, double lo, double hi, double ref)
{
    Shape s;
    const int c = static_cast<int>(std::lround((lo + hi) / 2.0));
    s.centreDb = db(m[static_cast<std::size_t>(c)] / ref);
    const auto bw = [&](double levelDb) {
        const double t = ref * std::pow(10.0, levelDb / kDb);
        int a = c, b = c;
        while (a > 0 && m[static_cast<std::size_t>(a - 1)] >= t) --a;
        while (b + 1 < static_cast<int>(m.size()) && m[static_cast<std::size_t>(b + 1)] >= t) ++b;
        return m[static_cast<std::size_t>(c)] >= t ? static_cast<double>(b - a) : 0.0;
    };
    s.bw6 = bw(-6.0);
    s.bw60 = bw(-60.0);
    const auto outside = [&](double guard) {
        double worst = 0.0;
        for (std::size_t f = 0; f < m.size(); ++f) {
            const double fd = static_cast<double>(f);
            if (fd <= lo - guard || fd >= hi + guard)
                worst = std::max(worst, m[f]);
        }
        return db(worst / ref);
    };
    s.at50 = outside(50.0);
    s.at100 = outside(100.0);
    return s;
}

// Onset: continuous tone after a noise-primed silence; first sample reaching
// half of the settled amplitude (settled = final quarter, not the overall
// peak, which on a linear-phase bandpass is Gibbs overshoot). ms from the
// tone's first input sample.
double onsetMs(WdspChannel::Mode mode, double lo, double hi, int taps, bool mp,
               double toneHz)
{
    std::string err;
    auto ch = WdspChannel::create(channelConfig(mode, lo, hi, taps, mp), &err);
    if (!ch)
        return -1.0;
    auto in = primer(16 * kBlk);
    in.resize(in.size() + 32 * kBlk);
    const std::size_t at = in.size();
    in.resize(at + 48 * kBlk);
    for (std::size_t k = at; k < in.size(); ++k) {
        // Negative baseband: the geometry WDSP runs in here (see
        // wdsp_channel_test's notch-attenuation test).
        const double ph = -2.0 * kPi * toneHz * static_cast<double>(k - at) / kFs;
        in[k] = {static_cast<float>(0.1 * std::cos(ph)), static_cast<float>(0.1 * std::sin(ph))};
    }
    std::vector<float> out;
    if (!run(*ch, in, out))
        return -1.0;
    double settled = 0.0;
    for (std::size_t k = out.size() - out.size() / 8; k < out.size(); ++k)
        settled = std::max(settled, static_cast<double>(std::fabs(out[k])));
    for (std::size_t k = at; k < out.size(); ++k) {
        if (std::fabs(out[k]) >= 0.5 * settled)
            return 1000.0 * static_cast<double>(k - at) / kFs;
    }
    return -1.0;
}

// ---- 3/4. through a real Hl2RxDsp ----------------------------------------

Hl2RxDsp::Config dspConfig(WdspChannel::Mode mode, double lo, double hi)
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = kFs;
    cfg.dspBlockSize = static_cast<int>(kBlk);
    cfg.fftSize = 256;
    cfg.mode = mode;
    cfg.filterLowHz = lo;
    cfg.filterHighHz = hi;
    cfg.agcMode = 0;
    cfg.maximumAgcGainDb = 39.0;
    cfg.blockForOutput = true;
    return cfg;
}

// A steady tone above centre in WIRE order (conjugate of the analytic
// convention, as hl2_rxdsp_unmute_return_test feeds it).
std::vector<std::complex<float>> wireTone(std::size_t n, std::size_t phase0, double hz)
{
    std::vector<std::complex<float>> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        const double ph = 2.0 * kPi * hz * static_cast<double>(k + phase0) / kFs;
        out[k] = 0.02f * std::complex<float>(static_cast<float>(std::cos(ph)),
                                             static_cast<float>(-std::sin(ph)));
    }
    return out;
}

void feed(Hl2RxDsp& dsp, const std::vector<std::complex<float>>& s)
{
    for (std::size_t off = 0; off < s.size(); off += kEp6BlockSamples) {
        const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, s.size() - off);
        dsp.processIqBlock(std::vector<std::complex<float>>(
            s.begin() + static_cast<std::ptrdiff_t>(off),
            s.begin() + static_cast<std::ptrdiff_t>(off + n)));
    }
}

struct Capture {
    std::vector<float> left;
    void attach(Hl2RxDsp& dsp)
    {
        QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                         [this](const std::vector<float>& pcm) {
            for (std::size_t i = 0; i < pcm.size(); i += 2)
                left.push_back(pcm[i]);
        });
    }
};

double rmsOf(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double s = 0.0;
    to = std::min(to, x.size());
    if (to <= from)
        return 0.0;
    for (std::size_t k = from; k < to; ++k)
        s += static_cast<double>(x[k]) * x[k];
    return std::sqrt(s / static_cast<double>(to - from));
}

constexpr double kTuneHz = 7'030'000.0;

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using M = WdspChannel::Mode;
    constexpr int kShort = Hl2RxDsp::kRxShortFilterTaps;
    // The length #5678 row 4.1 proposed. Measured alongside, NOT used: see
    // the negative control in 1c for why.
    constexpr int k2048 = 2048;
    constexpr int kLong = Hl2RxDsp::kRxFilterTaps;

    // ==== 1a. magnitude response per CW preset width, per length ==========
    // Reference level: the centre of the widest CW preset at the long length,
    // where every length is flat.
    double ref = 0.0;
    {
        const auto m = cwResponse(kPitchHz - 500.0, kPitchHz + 500.0, kLong);
        check(!m.empty(), "reference response measured");
        if (m.empty())
            return 1;
        ref = m[static_cast<std::size_t>(kPitchHz)];
    }
    const double widths[] = {50.0, 100.0, 150.0, 200.0, 250.0, 300.0, 400.0, 500.0};
    const int lengths[] = {k2048, kShort, kLong};
    Shape shapes[std::size(widths)][std::size(lengths)];
    std::printf("\n  MEASURED: CWU magnitude response, linear phase, centred on %.0f Hz\n"
                "  width  taps   centre dB   BW-6 Hz  BW-60 Hz  >=50Hz out dB  >=100Hz out dB\n",
                kPitchHz);
    for (std::size_t w = 0; w < std::size(widths); ++w) {
        for (std::size_t t = 0; t < std::size(lengths); ++t) {
            const double lo = kPitchHz - widths[w] / 2.0;
            const double hi = kPitchHz + widths[w] / 2.0;
            const auto m = cwResponse(lo, hi, lengths[t]);
            if (m.empty()) {
                check(false, "a response measurement ran");
                continue;
            }
            shapes[w][t] = shapeOf(m, lo, hi, ref);
            const Shape& s = shapes[w][t];
            std::printf("  %5.0f  %5d   %8.2f   %7.1f   %7.1f   %12.1f   %13.1f\n",
                        widths[w], lengths[t], s.centreDb, s.bw6, s.bw60, s.at50, s.at100);
        }
    }

    // ==== 1b. onset latency =================================================
    std::printf("\n  MEASURED: onset (half settled amplitude), noise-primed, AGC off\n");
    double cwOnset[std::size(lengths)] = {};
    for (std::size_t t = 0; t < std::size(lengths); ++t) {
        cwOnset[t] = onsetMs(M::Cwu, 550.0, 850.0, lengths[t], false, kPitchHz);
        std::printf("    CWU 550-850 Hz, %5d taps linear:   %7.2f ms\n", lengths[t], cwOnset[t]);
    }
    const double usbMpLong = onsetMs(M::Usb, 150.0, 3000.0, kLong, true, 1500.0);
    const double usbMpShort = onsetMs(M::Usb, 150.0, 3000.0, k2048, true, 1500.0);
    std::printf("    USB 150-3000 Hz, %5d taps minimum: %7.2f ms\n", kLong, usbMpLong);
    std::printf("    USB 150-3000 Hz, %5d taps minimum: %7.2f ms\n", k2048, usbMpShort);

    // ==== 1c. the thresholds against what each length realises =============
    //
    // "Realises the filter" = against the long filter at the same width:
    // centre gain within kGainTolDb, -6 dB bandwidth within kBwTol of the width
    // asked for, and everything 50 Hz or more outside the edges at least
    // kStopDb down -- the skirt is what a CW operator hears as a neighbour
    // 50 Hz off the filter edge. Every width the policy hands a length must
    // pass.
    constexpr double kGainTolDb = 0.5;
    constexpr double kBwTol = 0.10;
    constexpr double kStopDb = -60.0;
    const auto realises = [&](std::size_t w, std::size_t t) {
        const Shape& s = shapes[w][t];
        const Shape& l = shapes[w][std::size(lengths) - 1];
        return std::fabs(s.centreDb - l.centreDb) <= kGainTolDb
               && std::fabs(s.bw6 - l.bw6) <= kBwTol * widths[w]
               && s.at50 <= kStopDb;
    };
    bool policyHonest = true;
    for (std::size_t w = 0; w < std::size(widths); ++w) {
        const int picked = Hl2RxDsp::rxFilterTapsFor(M::Cwu, kPitchHz - widths[w] / 2.0,
                                                     kPitchHz + widths[w] / 2.0, 0);
        for (std::size_t t = 0; t < std::size(lengths); ++t) {
            if (lengths[t] == picked && !realises(w, t)) {
                std::fprintf(stderr, "    %.0f Hz handed %d taps, which do not realise it\n",
                             widths[w], picked);
                policyHonest = false;
            }
        }
    }
    check(policyHonest,
          "every CW width is handed a length that realises it (gain, -6 dB width, "
          "-60 dB from 50 Hz outside the edges) as measured above");
    // NEGATIVE CONTROL, and the reason the short length is not 2048: at 2048
    // taps the skirt 50 Hz outside the edge is ~-33 dB at EVERY width, so no
    // CW width is realised and no threshold could make 2048 honest.
    bool no2048 = true;
    for (std::size_t w = 0; w < std::size(widths); ++w)
        no2048 = no2048 && !realises(w, 0);
    check(no2048, "2048 taps realise no CW width (skirt 50 Hz out is not -60 dB) -- "
                  "so the criterion can fail, and 2048 is rightly unused");
    // And the long filter itself realises every preset, or the comparison is moot.
    bool longOk = true;
    for (std::size_t w = 0; w < std::size(widths); ++w)
        longOk = longOk && shapes[w][std::size(lengths) - 1].at50 <= kStopDb;
    check(longOk, "control: the 8192-tap filter is >= 60 dB down 50 Hz outside every width");

    // Latency: what the short length buys in CW. ASSERTED on the impulse
    // response's centre of energy, which for a symmetric FIR is the group
    // delay exactly and does not depend on the skirt; the onset above is the
    // operator-facing figure but includes each length's own rise, which is
    // not the same shape at 4096 and 8192.
    const double cShort = energyCentre(cwImpulse(550.0, 850.0, kShort));
    const double cLong = energyCentre(cwImpulse(550.0, 850.0, kLong));
    std::printf("    CWU 550-850 Hz impulse centre of energy: %.2f samples at %d, "
                "%.2f at %d, delta %.2f (expected %d)\n",
                cShort, kShort, cLong, kLong, cLong - cShort, (kLong - kShort) / 2);
    check(cShort > 0.0 && cLong > 0.0, "CW impulse responses measured");
    check(std::fabs((cLong - cShort) - (kLong - kShort) / 2.0) < 2.0,
          "CW: 8192 -> 4096 taps moves the group delay by (8192-4096)/2 = 2048 "
          "samples (42.67 ms), within 2 samples");
    // The onset delta, loosely: 2048 samples PLUS the difference between the
    // two lengths' rise to half amplitude, which is not zero in principle
    // because their skirts differ. 3 ms (144 samples) is the bound review
    // proposed; it still tells 4096 from 2048 or 8192 by more than 18 ms,
    // and the exact figure is carried by the centre-of-energy check above.
    check(cwOnset[1] > 0.0 && cwOnset[2] > 0.0
              && std::fabs((cwOnset[2] - cwOnset[1]) - 1000.0 * (kLong - kShort) / 2.0 / kFs) < 3.0,
          "CW: the half-amplitude onset moves by the same 42.67 ms, within 3 ms");
    // Why the policy leaves every other mode long: at minimum phase the length
    // barely moves the onset.
    check(usbMpLong > 0.0 && usbMpShort > 0.0 && std::fabs(usbMpLong - usbMpShort) < 2.0,
          "outside CW (minimum phase) 8192 vs 2048 taps differ by < 2 ms of onset");

    // ==== 2. the policy, as a table ========================================
    {
        bool mp = true;
        for (M m : {M::Lsb, M::Usb, M::Dsb, M::Fm, M::Am, M::Digu, M::Spec, M::Digl,
                    M::Sam, M::Drm, M::Wbfm})
            mp = mp && Hl2RxDsp::rxFilterTapsFor(m, 150.0, 3000.0, 0) == kLong
                    && Hl2RxDsp::rxFilterTapsFor(m, 650.0, 750.0, 0) == kLong;
        check(mp, "policy: every mode outside CW keeps the long filter");
        check(Hl2RxDsp::rxFilterTapsFor(M::Cwu, 550.0, 850.0, 0) == kShort
              && Hl2RxDsp::rxFilterTapsFor(M::Cwl, -850.0, -550.0, 0) == kShort,
              "policy: CW 300 Hz without a notch runs short (both sidebands)");
        check(Hl2RxDsp::rxFilterTapsFor(M::Cwu, -250.0, 250.0, 0) == kShort,
              "policy: CW's default 500 Hz passband runs short");
        check(Hl2RxDsp::rxFilterTapsFor(M::Cwu, 550.0, 850.0, 1) == kLong,
              "policy: any notch selects the long filter");
        check(Hl2RxDsp::rxFilterTapsFor(M::Cwu, 675.0, 725.0, 0) == kLong,
              "policy: a 50 Hz CW filter selects the long filter");
        check(Hl2RxDsp::rxFilterTapsFor(M::Cwu, 650.0, 750.0, 0) == kShort,
              "policy: a 100 Hz CW filter (narrowest preset above 50) runs short");
        // Hysteresis: at the threshold from the long length, stay long; from
        // the short length itself, stay short.
        const double t = Hl2RxDsp::kRxShortTapsMinWidthHz;
        check(Hl2RxDsp::rxFilterTapsFor(M::Cwu, 0.0, t, 0, kShort) == kShort
              && Hl2RxDsp::rxFilterTapsFor(M::Cwu, 0.0, t, 0, kLong) == kLong
              && Hl2RxDsp::rxFilterTapsFor(M::Cwu, 0.0, t + Hl2RxDsp::kRxTapsHysteresisHz, 0, kLong) == kShort,
              "policy: shortening needs the hysteresis margin, staying does not");
        check(Hl2RxDsp::rxFilterTapsFor(M::Cwu, 0.0, t - 1.0, 0, kShort) != kShort,
              "policy: below the threshold the short length is left even from short");
    }

    // ==== 3. the wiring on a live Hl2RxDsp ==================================
    {
        Hl2RxDsp dsp;
        std::string err;
        check(dsp.configure(dspConfig(M::Cwu, 550.0, 850.0), &err), "CWU chain configures");
        check(dsp.rxFilterTapsInForce() == kShort, "CWU 300 Hz opens at the short length");
        dsp.setMode(M::Usb);
        check(dsp.rxFilterTapsInForce() == kLong, "setMode(USB) raises it");
        dsp.setMode(M::Cwu);
        check(dsp.rxFilterTapsInForce() == kShort, "setMode(CWU) lowers it again");
        dsp.setFilter(675.0, 725.0);
        check(dsp.rxFilterTapsInForce() == kLong, "setFilter(50 Hz) raises it");
        dsp.setFilter(550.0, 850.0);
        check(dsp.rxFilterTapsInForce() == kShort, "setFilter(300 Hz) lowers it");

        dsp.setNotchTuneFrequency(kTuneHz);
        dsp.addNotch(0, kTuneHz + 1500.0, 50.0, true);
        check(dsp.rxFilterTapsInForce() == kLong, "the first notch raises it");
        check(dsp.minimumNotchWidthInForceHz() <= 50.0 + 1e-9,
              "... so the channel's notch floor is 50 Hz while a notch exists");
        dsp.addNotch(1, kTuneHz + 1800.0, 50.0, true);
        dsp.removeNotch(1);
        check(dsp.rxFilterTapsInForce() == kLong, "removing one of two notches keeps it");
        dsp.removeNotch(0);
        check(dsp.rxFilterTapsInForce() == kShort, "removing the last notch gives it back");
        check(dsp.notchCount() == 0 && dsp.wdspNotchCount() == 0, "mirror and WDSP agree: empty");

        // Seeding: clear + replay of the same set must not bounce the length.
        dsp.addNotch(0, kTuneHz + 1500.0, 50.0, true);
        dsp.clearNotches();
        check(dsp.rxFilterTapsInForce() == kLong,
              "clearNotches() (seed's first half) does not lower the length");
        dsp.addNotch(0, kTuneHz + 1500.0, 50.0, true);
        dsp.setNotchesEnabled(true);
        check(dsp.rxFilterTapsInForce() == kLong, "a reseeded set keeps the long length");
        dsp.clearNotches();
        dsp.setNotchesEnabled(true);
        check(dsp.rxFilterTapsInForce() == kShort,
              "a seed that leaves the set empty returns the length at its closing enable");

        // Rebuild: built for no notches, installed with one in the mirror.
        dsp.addNotch(0, kTuneHz + 1500.0, 50.0, true);
        const Hl2RxDsp::Config cfg = dspConfig(M::Cwu, 550.0, 850.0);
        dsp.beginRebuild(cfg);
        check(dsp.installRebuiltChannel(Hl2RxDsp::buildChannel(cfg, false, 0)),
              "rebuild installs");
        check(dsp.rxFilterTapsInForce() == kLong && dsp.wdspNotchCount() == 1,
              "rebuild: the replayed notch set holds the long length");
        check(dsp.minimumNotchWidthInForceHz() <= 50.0 + 1e-9,
              "rebuild: the replayed notch is at the 50 Hz floor");
    }
    {
        // Rebuild with NO notches, where the mode moved while the build ran:
        // the chain is built for the Config it was handed, so only
        // installChannel()'s own applyFilterTaps() can correct its length.
        const Hl2RxDsp::Config cwCfg = dspConfig(M::Cwu, 550.0, 850.0);
        const Hl2RxDsp::Config usbCfg = dspConfig(M::Usb, 150.0, 3000.0);
        {
            Hl2RxDsp dsp;
            std::string err;
            check(dsp.configure(cwCfg, &err), "rebuild CW->USB: configures");
            dsp.beginRebuild(cwCfg);
            dsp.setMode(M::Usb);   // deferred: a rebuild is in flight
            check(dsp.installRebuiltChannel(Hl2RxDsp::buildChannel(cwCfg, false, 0)),
                  "rebuild CW->USB: installs");
            check(dsp.rxFilterTapsInForce() == kLong,
                  "rebuild: built for CW (4096), USB set mid-build -> installed at 8192");
        }
        {
            Hl2RxDsp dsp;
            std::string err;
            check(dsp.configure(usbCfg, &err), "rebuild USB->CW: configures");
            dsp.beginRebuild(usbCfg);
            dsp.setMode(M::Cwu);
            dsp.setFilter(550.0, 850.0);
            check(dsp.installRebuiltChannel(Hl2RxDsp::buildChannel(usbCfg, false, 0)),
                  "rebuild USB->CW: installs");
            check(dsp.rxFilterTapsInForce() == kShort,
                  "rebuild: built for USB (8192), CW 300 Hz set mid-build -> installed at 4096");
        }
    }
    {
        // A rebuild keeps the width hysteresis: a 110 Hz CW passband held long
        // (it was narrowed to 99 Hz first) stays long on the fresh chain, which
        // buildChannel() opened short because it has no length to compare to.
        Hl2RxDsp dsp;
        std::string err;
        check(dsp.configure(dspConfig(M::Cwu, 550.0, 850.0), &err), "rebuild hysteresis: configures");
        dsp.setFilter(650.0, 749.0);
        dsp.setFilter(645.0, 755.0);
        check(dsp.rxFilterTapsInForce() == kLong,
              "rebuild hysteresis: 110 Hz after 99 Hz is held long");
        const Hl2RxDsp::Config cfg = dspConfig(M::Cwu, 645.0, 755.0);
        dsp.beginRebuild(cfg);
        check(dsp.installRebuiltChannel(Hl2RxDsp::buildChannel(cfg, false, 0)),
              "rebuild hysteresis: installs");
        check(dsp.rxFilterTapsInForce() == kLong,
              "rebuild hysteresis: the fresh chain keeps the long length the old one held");
        dsp.setFilter(550.0, 850.0);
        check(dsp.rxFilterTapsInForce() == kShort,
              "rebuild hysteresis: a 300 Hz passband still shortens after it");
    }
    {
        // A raise that fails refuses the notch: on the 4096 filter WDSP would
        // widen it to 100 Hz, so neither WDSP nor the mirror may hold it. Once
        // the raise works again, the next notch lands long.
        Hl2RxDsp dsp;
        std::string err;
        check(dsp.configure(dspConfig(M::Cwu, 550.0, 850.0), &err), "refused raise: configures");
        dsp.setNotchTuneFrequency(kTuneHz);
        dsp.setRefuseFilterTapsChangesForTest(true);
        dsp.addNotch(0, kTuneHz + 1500.0, 50.0, true);
        check(dsp.rxFilterTapsInForce() == kShort, "refused raise: the length stayed at 4096");
        check(dsp.wdspNotchCount() == 0 && dsp.notchCount() == 0,
              "refused raise: the notch is NOT applied at 4096 (WDSP and mirror both empty)");
        dsp.setRefuseFilterTapsChangesForTest(false);
        dsp.addNotch(0, kTuneHz + 1500.0, 50.0, true);
        check(dsp.rxFilterTapsInForce() == kLong && dsp.wdspNotchCount() == 1
                  && dsp.notchCount() == 1,
              "refused raise: the next attempt raises and lands the notch");
    }

    // ==== 4. the live switch, and the notch across it =====================
    {
        // A tone at the pitch through CWU 550-850, steady at the short length;
        // then a notch far outside the passband forces the long length
        // mid-stream. The notch cannot touch the tone, so any change in the
        // audio is the switch itself.
        Hl2RxDsp dsp;
        std::string err;
        check(dsp.configure(dspConfig(M::Cwu, 550.0, 850.0), &err), "switch leg configures");
        dsp.setNotchTuneFrequency(kTuneHz);
        Capture cap;
        cap.attach(dsp);
        std::size_t ph = 0;
        feed(dsp, wireTone(96 * kBlk, ph, kPitchHz));
        ph += 96 * kBlk;
        const std::size_t before = cap.left.size();
        const double levelBefore = rmsOf(cap.left, before - 24 * kBlk, before);
        dsp.addNotch(0, kTuneHz + 2500.0, 50.0, true);
        const bool switched = dsp.rxFilterTapsInForce() == kLong;
        feed(dsp, wireTone(96 * kBlk, ph, kPitchHz));
        ph += 96 * kBlk;
        const double levelAfter = rmsOf(cap.left, cap.left.size() - 24 * kBlk, cap.left.size());

        bool finite = true;
        double peak = 0.0;
        for (std::size_t k = before; k < cap.left.size(); ++k) {
            finite = finite && std::isfinite(cap.left[k]);
            peak = std::max(peak, static_cast<double>(std::fabs(cap.left[k])));
        }
        // The refill: from the switch, how long the tone's 10 ms RMS sits
        // more than 6 dB under its settled level.
        const std::size_t win = kFs / 100;
        std::size_t lastLow = before;
        for (std::size_t k = before; k + win <= cap.left.size(); k += win / 10) {
            if (rmsOf(cap.left, k, k + win) < levelBefore * 0.5)
                lastLow = k + win;
        }
        const double gapMs = 1000.0 * static_cast<double>(lastLow - before) / kFs;
        const double steadyPeak = levelBefore * std::sqrt(2.0);
        std::printf("\n  MEASURED: live 4096 -> 8192 switch under a steady tone:\n"
                    "    level before %.5f, after %.5f (%.2f dB); peak %.5f = %.2f x steady;"
                    " below -6 dB for %.1f ms\n",
                    levelBefore, levelAfter, db(levelAfter / levelBefore), peak,
                    peak / steadyPeak, gapMs);
        check(switched, "switch: the far notch raised the length mid-stream");
        check(finite, "switch: no NaN or Inf in the audio across it");
        check(peak < 1.25 * steadyPeak,
              "switch: no spike -- the peak stays within the linear-phase filter's own "
              "overshoot of the steady tone");
        check(std::fabs(db(levelAfter / levelBefore)) < 0.5,
              "switch: the settled level is the same after it");
        check(gapMs < 1000.0 * kLong / kFs,
              "switch: the refill is shorter than one long-filter support (170.7 ms)");
    }
    {
        // Notch depth: a 50 Hz notch placed ON the tone. Once on a chain that
        // switches to take it (CW 300 Hz, short), once on a chain already long
        // before the notch (USB), and a mirror control. The depth must match:
        // the notch is not widened or weakened by arriving across the switch.
        const auto depthDb = [&](M mode, double lo, double hi, double notchRf, int* tapsSeen) {
            Hl2RxDsp dsp;
            std::string err;
            if (!dsp.configure(dspConfig(mode, lo, hi), &err))
                return 999.0;
            dsp.setNotchTuneFrequency(kTuneHz);
            Capture cap;
            cap.attach(dsp);
            std::size_t ph = 0;
            feed(dsp, wireTone(96 * kBlk, ph, kPitchHz));
            ph += 96 * kBlk;
            const double open = rmsOf(cap.left, cap.left.size() - 24 * kBlk, cap.left.size());
            dsp.addNotch(0, notchRf, 50.0, true);
            *tapsSeen = dsp.rxFilterTapsInForce();
            feed(dsp, wireTone(96 * kBlk, ph, kPitchHz));
            const double notched = rmsOf(cap.left, cap.left.size() - 24 * kBlk, cap.left.size());
            return db(notched / open);
        };
        int tapsCw = 0, tapsUsb = 0, tapsMirror = 0;
        const double cw = depthDb(M::Cwu, 550.0, 850.0, kTuneHz + kPitchHz, &tapsCw);
        const double usb = depthDb(M::Usb, 150.0, 3000.0, kTuneHz + kPitchHz, &tapsUsb);
        const double mirror = depthDb(M::Cwu, 550.0, 850.0, kTuneHz - kPitchHz, &tapsMirror);
        std::printf("\n  MEASURED: 50 Hz notch on a %.0f Hz tone -- CWU across the switch %.1f dB"
                    " (%d taps), USB already long %.1f dB (%d taps), CWU mirror %.2f dB\n",
                    kPitchHz, cw, tapsCw, usb, tapsUsb, mirror);
        check(tapsCw == kLong && tapsUsb == kLong, "notch depth: both chains run long with the notch");
        check(cw < -40.0, "notch depth: the notch placed across the switch is over 40 dB deep");
        check(std::fabs(cw - usb) < 6.0,
              "notch depth: within 6 dB of the same notch on a chain that was already long");
        check(std::fabs(mirror) < 0.5, "control: a notch on the mirror frequency leaves the tone");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_rxdsp_adaptive_taps_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
