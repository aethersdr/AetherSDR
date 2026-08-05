// HL2 AM/SAM DC-pedestal regression test.
//
// WDSP's AM/SAM detector is an ENVELOPE detector — amd.c emits
// sqrt(I^2 + Q^2), strictly non-negative — so the carrier lands in the
// demodulated audio as a DC pedestal. Neither `levelfade` (which cancels
// fading and deliberately HOLDS that pedestal) nor the symmetric {-4000,+4000}
// AM passband removes it, and AetherSDR has no other RX DC blocker. Left in,
// the pedestal eats headroom, biases wcpAGC, and offsets every zero-referenced
// consumer — the WAVE applet mirrors `peak`/`rms` about a hard centreline, so a
// DC-shifted trace draws a second, INVERTED phantom copy of itself.
//
// Two things make this test able to fail against the unfixed chain:
//
//   1. It feeds a REAL amplitude-modulated carrier, A*(1 + m*cos(wm.t)), in
//      WIRE ORDER (the HPSDR wire is the conjugate of the analytic convention,
//      so a carrier above the NCO is exp(-j.w.t)). A textbook constant-envelope
//      tone has no modulation to distinguish from its own DC.
//   2. It measures the SETTLED tail. `dc_insert` is a 1.4 s one-pole (RXA.c
//      tauI), so the pedestal ramps in from zero — a short burst shows almost
//      no DC even with the blocker removed, and would pass either way.
//
// The assertion is a RATIO of DC to AC, not an absolute level, because
// wcpAGC applies the same gain to both and the absolute scale is arbitrary.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <span>
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

namespace {

struct AudioStats {
    double mean = 0.0;    // DC pedestal
    double acRms = 0.0;   // RMS about that mean — the actual program material
};

// Drive `dsp` with `seconds` of amplitude-modulated carrier and return the
// statistics of the settled TAIL of the demodulated audio.
AudioStats runAm(Hl2RxDsp& dsp,
                 int inputRateHz,
                 double seconds,
                 double tailSeconds,
                 double carrierOffsetHz,
                 double modHz,
                 double modDepth)
{
    std::vector<float> audio;
    const auto conn = QObject::connect(
        &dsp, &Hl2RxDsp::audioReady, &dsp,
        [&](const std::vector<float>& pcm) {
            // Left channel only; AM writes both channels identically.
            for (std::size_t k = 0; k < pcm.size(); k += 2)
                audio.push_back(pcm[k]);
        });

    const auto total = static_cast<int>(seconds * inputRateHz);
    std::vector<std::complex<float>> stream(static_cast<std::size_t>(total));
    for (int n = 0; n < total; ++n) {
        const double t = static_cast<double>(n) / inputRateHz;
        const double env = 0.3 * (1.0 + modDepth * std::cos(2.0 * std::numbers::pi * modHz * t));
        // WIRE ORDER: negative sine puts the carrier ABOVE the NCO.
        const double ph = 2.0 * std::numbers::pi * carrierOffsetHz * t;
        stream[static_cast<std::size_t>(n)] =
            std::complex<float>(static_cast<float>(env * std::cos(ph)),
                                static_cast<float>(-env * std::sin(ph)));
    }

    std::span<const std::complex<float>> s(stream);
    for (std::size_t off = 0; off < s.size(); off += kEp6BlockSamples) {
        const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, s.size() - off);
        const auto sub = s.subspan(off, n);
        dsp.processIqBlock(std::vector<std::complex<float>>(sub.begin(), sub.end()));
    }
    QObject::disconnect(conn);

    AudioStats st;
    if (audio.empty())
        return st;

    // Settled tail only — see the dc_insert note in the file header.
    const auto tail = std::min<std::size_t>(
        audio.size(),
        static_cast<std::size_t>(tailSeconds * audio.size() / seconds));
    const std::size_t start = audio.size() - tail;

    double sum = 0.0;
    for (std::size_t i = start; i < audio.size(); ++i)
        sum += audio[i];
    st.mean = sum / static_cast<double>(tail);

    double sumSq = 0.0;
    for (std::size_t i = start; i < audio.size(); ++i) {
        const double d = audio[i] - st.mean;
        sumSq += d * d;
    }
    st.acRms = std::sqrt(sumSq / static_cast<double>(tail));
    return st;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    constexpr int kInputRate = 48000;
    constexpr double kSeconds = 4.0;      // > 2x the 1.4 s dc_insert time constant
    constexpr double kTail = 1.0;         // measure only the settled last second
    constexpr double kModHz = 400.0;
    constexpr double kModDepth = 0.5;

    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kInputRate;
    cfg.audioSampleRateHz = 24000;        // the shipping audio rate
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.blockForOutput = true;            // deterministic for an offline burst feed

    // ---- AM: the mode that carries the pedestal ----
    {
        Hl2RxDsp dsp;
        Hl2RxDsp::Config c = cfg;
        c.mode = WdspChannel::Mode::Am;
        c.filterLowHz = -4000.0;          // symmetric, as defaultPassbandForMode gives AM
        c.filterHighHz = 4000.0;
        std::string err;
        check(dsp.configure(c, &err), err.empty() ? "AM configures" : err.c_str());

        const AudioStats st = runAm(dsp, kInputRate, kSeconds, kTail,
                                    /*carrierOffsetHz=*/1000.0, kModHz, kModDepth);

        check(st.acRms > 1e-4,
              "AM demod produced non-silent modulation (the blocker did not eat the audio)");

        // The discriminator. With m = 0.5 the envelope is A*(1 + 0.5cos), so
        // undetected DC gives mean/acRms = A / (0.5A/sqrt2) = 2.83. The blocker
        // must drive that ratio to roughly zero. 0.10 sits an order of magnitude
        // below the unfixed value, so this cannot pass on a pedestal.
        const double ratio = st.acRms > 0.0 ? std::fabs(st.mean) / st.acRms : 1e9;
        static_assert(kModDepth == 0.5, "the 2.83 unfixed ratio below assumes m = 0.5");
        check(ratio < 0.10,
              "AM audio is zero-mean: no carrier DC pedestal survives to the audio");
        if (ratio >= 0.10) {
            std::fprintf(stderr, "  DC/AC ratio = %.3f (mean %.6f, acRms %.6f);"
                                 " unfixed chain measures ~2.83\n",
                         ratio, st.mean, st.acRms);
        }
    }

    // ---- SAM: same detector family, same pedestal ----
    {
        Hl2RxDsp dsp;
        Hl2RxDsp::Config c = cfg;
        c.mode = WdspChannel::Mode::Sam;
        c.filterLowHz = -4000.0;
        c.filterHighHz = 4000.0;
        std::string err;
        check(dsp.configure(c, &err), err.empty() ? "SAM configures" : err.c_str());

        const AudioStats st = runAm(dsp, kInputRate, kSeconds, kTail,
                                    /*carrierOffsetHz=*/1000.0, kModHz, kModDepth);

        check(st.acRms > 1e-4, "SAM demod produced non-silent modulation");
        const double ratio = st.acRms > 0.0 ? std::fabs(st.mean) / st.acRms : 1e9;
        check(ratio < 0.10, "SAM audio is zero-mean: no carrier DC pedestal");
        if (ratio >= 0.10) {
            std::fprintf(stderr, "  SAM DC/AC ratio = %.3f (mean %.6f, acRms %.6f)\n",
                         ratio, st.mean, st.acRms);
        }
    }

    // ---- USB: the blocker must be a no-op on a mode that was already correct ----
    //
    // The reported symptom was AM/SAM-only, so the fix has to leave SSB
    // untouched. A 20 Hz corner is far below the 100 Hz the USB passband starts
    // at, so the modulation must survive at full strength.
    {
        Hl2RxDsp dsp;
        Hl2RxDsp::Config c = cfg;
        c.mode = WdspChannel::Mode::Usb;
        c.filterLowHz = 100.0;
        c.filterHighHz = 2900.0;
        std::string err;
        check(dsp.configure(c, &err), err.empty() ? "USB configures" : err.c_str());

        const AudioStats st = runAm(dsp, kInputRate, kSeconds, kTail,
                                    /*carrierOffsetHz=*/1000.0, kModHz, kModDepth);

        check(st.acRms > 1e-4, "USB demod still produces audio through the DC blocker");
        const double ratio = st.acRms > 0.0 ? std::fabs(st.mean) / st.acRms : 1e9;
        check(ratio < 0.10, "USB audio remains zero-mean (it always was)");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_am_dcblock_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
