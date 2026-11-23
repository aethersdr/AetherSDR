// HL2 multi-slice speaker mix: headroom at high AGC ceilings.
//
// Two real Hl2RxDsp chains, offline and sample-counted (blockForOutput, no
// socket, no audio device, no wall clock), feed Hl2Backend::mixReceiverAudio().
// Slice A: a 1 kHz tone at -25.5 dBFS over -120 dBFS/Hz noise; slice B: that
// noise alone, independent. With the AGC ceiling high enough the AGC levels
// slice B's noise to about -14 dBFS RMS, and its peaks added to a full-level
// slice A pass full scale.
//
//   1. ONE SLICE IS UNTOUCHED: the fast path (unity, centre) and the summing
//      path (gain 0.5) both pass a 90 dB slice bit-identical.
//   2. A MIX THAT FITS IS UNTOUCHED: at 60 dB, once the AGC has settled, the
//      plain float sum stays under full scale and the published mix equals it
//      bit for bit.
//   3. POSITIVE CONTROL: at 70 and 90 dB the plain sum of the two slices DOES
//      pass full scale, on many samples, so check 4 is not vacuous.
//   4. NO SAMPLE OF THE PUBLISHED MIX REACHES FULL SCALE at 70 dB and at 90 dB
//      (AGC-T 100 through Hl2DbReference), settled or during the AGC's
//      attack, and the mix keeps its level (RMS within 3 dB of the plain sum;
//      scaling by 1/2 would cost 6.02 dB).

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2DbReference.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace AetherSDR::hl2 {
struct Hl2MixHeadroomTestAccess {
    static void start(Hl2Backend& backend, Hl2RxDsp& first, Hl2RxDsp& second)
    {
        backend.m_rx.resize(2);
        backend.m_rx[0].dsp = &first;
        backend.m_rx[1].dsp = &second;
        backend.m_connected = true;
        emit backend.connected();
    }
    static void setSecondMuted(Hl2Backend& backend, bool muted)
    {
        backend.m_rx[1].audioMuted = muted;
    }
    static void setFirstGain(Hl2Backend& backend, float gain)
    {
        backend.m_rx[0].audioGain = gain;
    }
    static void mix(Hl2Backend& backend, int index, const std::vector<float>& pcm)
    {
        backend.mixReceiverAudio(index, pcm);
    }
    static void finish(Hl2Backend& backend)
    {
        // Stack-owned, unconfigured DSP objects: never hand them to teardown.
        backend.m_rx.clear();
        backend.m_connected = false;
        backend.retirePcmStreams();
    }
};
} // namespace AetherSDR::hl2

using namespace AetherSDR;
using namespace AetherSDR::hl2;

namespace {

int g_failures = 0;
void check(bool cond, const std::string& what)
{
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "ok" : "FAIL", what.c_str());
    if (!cond)
        ++g_failures;
}

constexpr double kPi = 3.14159265358979323846;
constexpr int kFs = 48000;
constexpr double kSettleSeconds = 3.0;
constexpr double kMeasureSeconds = 5.0;

using Blocks = std::vector<std::vector<float>>;   // audioReady blocks, interleaved L,R

// One slice's audio for the whole run. Noise in dBFS/Hz (0 dBFS = |z| = 1).
Blocks runSlice(double ceilingDb, double toneDbfs, double noiseDbPerHz, std::uint64_t seed)
{
    Blocks out;
    Hl2RxDsp dsp;
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = 24000;               // what Hl2Backend configures
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.filterLowHz = 100;
    cfg.filterHighHz = 2900;
    cfg.agcMode = 3;                             // medium
    cfg.maximumAgcGainDb = ceilingDb;
    cfg.blockForOutput = true;
    std::string err;
    if (!dsp.configure(cfg, &err)) {
        check(false, "Hl2RxDsp configures: " + err);
        return out;
    }
    QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                     [&out](const std::vector<float>& pcm) { out.push_back(pcm); });

    std::mt19937_64 rng(seed);
    auto uni = [&] { return (static_cast<double>(rng() >> 11) + 0.5) * 0x1p-53; };
    const double ns = std::sqrt(std::pow(10.0, noiseDbPerHz / 10.0) * kFs / 2.0);
    const double a = toneDbfs > -299 ? std::pow(10.0, toneDbfs / 20.0) : 0.0;
    const long total = std::lround((kSettleSeconds + kMeasureSeconds) * kFs);
    auto q24 = [](double x) {   // 24-bit EP6 sample, saturating
        return static_cast<float>(std::clamp(std::nearbyint(x * 8388608.0),
                                             -8388608.0, 8388607.0) / 8388608.0);
    };
    std::vector<std::complex<float>> blk;
    for (long n = 0; n < total; n += kEp6BlockSamples) {
        const long len = std::min<long>(kEp6BlockSamples, total - n);
        blk.resize(static_cast<std::size_t>(len));
        for (long k = 0; k < len; ++k) {
            const double ph = 2 * kPi * 1000.0 * static_cast<double>(n + k) / kFs;
            std::complex<double> z(a * std::cos(ph), a * std::sin(ph));
            const double u = std::sqrt(-2.0 * std::log(uni())), v = 2 * kPi * uni();
            z += ns * std::complex<double>(u * std::cos(v), u * std::sin(v));
            // The HPSDR wire is the conjugate of the analytic convention.
            blk[static_cast<std::size_t>(k)] = {q24(z.real()), q24(-z.imag())};
        }
        dsp.processIqBlock(blk);
    }
    return out;
}

std::vector<float> flat(const Blocks& b)
{
    std::vector<float> v;
    for (const auto& x : b)
        v.insert(v.end(), x.begin(), x.end());
    return v;
}

// Runs blocks through the real mixer and returns what it published.
std::vector<float> throughMixer(const Blocks& a, const Blocks* b, float gainA = 1.0f)
{
    Hl2Backend backend;
    Hl2RxDsp first;
    Hl2RxDsp second;
    Hl2MixHeadroomTestAccess::start(backend, first, second);
    Hl2MixHeadroomTestAccess::setSecondMuted(backend, b == nullptr);
    Hl2MixHeadroomTestAccess::setFirstGain(backend, gainA);
    std::vector<float> out;
    QObject::connect(&backend, &IRadioBackend::audioFrameReady, &backend,
                     [&out](const PcmFrame& frame) {
        const QByteArray bytes = frame.legacyStereo24();
        const std::size_t n = static_cast<std::size_t>(bytes.size()) / sizeof(float);
        const std::size_t at = out.size();
        out.resize(at + n);
        std::memcpy(out.data() + at, bytes.constData(), n * sizeof(float));
    });
    const std::size_t blocks = b ? std::min(a.size(), b->size()) : a.size();
    for (std::size_t i = 0; i < blocks; ++i) {
        Hl2MixHeadroomTestAccess::mix(backend, 0, a[i]);
        if (b)
            Hl2MixHeadroomTestAccess::mix(backend, 1, (*b)[i]);
    }
    Hl2MixHeadroomTestAccess::finish(backend);
    return out;
}

struct Stat { long atFullScale = 0; long n = 0; double peak = 0; double rms = 0; };
Stat statOf(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    Stat s;
    double sum = 0;
    for (std::size_t k = from; k < std::min(to, x.size()); ++k) {
        const double v = std::fabs(static_cast<double>(x[k]));
        s.peak = std::max(s.peak, v);
        if (std::fabs(x[k]) >= 1.0f)
            ++s.atFullScale;
        sum += v * v;
        ++s.n;
    }
    s.rms = s.n ? std::sqrt(sum / s.n) : 0;
    return s;
}
double db(double x) { return x > 0 ? 20.0 * std::log10(x) : -300.0; }

constexpr double kToneDbfs = -25.5;
constexpr double kNoiseDbPerHz = -120.0;
constexpr std::uint64_t kSeedA = 21, kSeedB = 1021;

void oneSliceUntouched()
{
    const Blocks a = runSlice(90.0, kToneDbfs, kNoiseDbPerHz, kSeedA);
    const std::vector<float> in = flat(a);
    const std::vector<float> fast = throughMixer(a, nullptr);
    check(!in.empty() && fast == in, "one slice, unity, centre (fast path): bit-identical at 90 dB");
    std::vector<float> half(in.size());
    for (std::size_t k = 0; k < in.size(); ++k)
        half[k] = in[k] * 0.5f;
    const std::vector<float> summed = throughMixer(a, nullptr, 0.5f);
    check(!in.empty() && summed == half,
          "one slice at gain 0.5 (summing path): bit-identical to input x 0.5 at 90 dB");
    const Stat s = statOf(in, 0, in.size());
    std::printf("info: one slice at 90 dB: peak %.3f dBFS, %ld of %ld samples at full scale\n",
                db(s.peak), s.atFullScale, s.n);
    check(s.atFullScale == 0, "one slice alone never reaches full scale at 90 dB");
}

void twoSlices(double ceilingDb, bool expectOverload)
{
    const Blocks a = runSlice(ceilingDb, kToneDbfs, kNoiseDbPerHz, kSeedA);
    const Blocks b = runSlice(ceilingDb, -300.0, kNoiseDbPerHz, kSeedB);
    const std::vector<float> fa = flat(a), fb = flat(b);
    const std::size_t n = std::min(fa.size(), fb.size()) & ~std::size_t{1};
    std::vector<float> plain(n);
    for (std::size_t k = 0; k < n; ++k)
        plain[k] = 0.0f + fa[k] + fb[k];
    const std::vector<float> mixed = throughMixer(a, &b);
    const std::size_t from = static_cast<std::size_t>(kSettleSeconds * 24000) * 2;
    const Stat p = statOf(plain, from, n);
    const Stat m = statOf(mixed, from, n);
    char tag[64];
    std::snprintf(tag, sizeof tag, "two slices at %.0f dB", ceilingDb);
    std::printf("info: %s: plain sum peak %.3f dBFS, %ld of %ld samples >= full scale, "
                "rms %.3f dBFS; published mix peak %.3f dBFS, %ld at full scale, rms %.3f dBFS\n",
                tag, db(p.peak), p.atFullScale, p.n, db(p.rms), db(m.peak), m.atFullScale,
                db(m.rms));
    check(mixed.size() >= n, std::string(tag) + ": the mixer published every frame");
    const Stat pWhole = statOf(plain, 0, n);
    const Stat mWhole = statOf(mixed, 0, n);
    std::printf("info: %s, whole run incl. the AGC's attack: plain sum %ld, published mix %ld "
                "samples at full scale\n", tag, pWhole.atFullScale, mWhole.atFullScale);
    if (!expectOverload) {
        // Measured part only: the AGC's attack in the first second overshoots,
        // and there even this plain sum passes full scale.
        std::size_t diff = from;
        while (diff < n && diff < mixed.size() && plain[diff] == mixed[diff])
            ++diff;
        if (diff < n)
            std::printf("info: %s: first difference at sample %zu\n", tag, diff);
        check(p.atFullScale == 0, std::string(tag) + ": the plain sum fits under full scale");
        check(mixed.size() >= n && diff == n,
              std::string(tag) + ": a mix that fits is published bit-identical to the plain sum");
        return;
    }
    check(p.atFullScale > 100,
          std::string(tag) + ": positive control, the plain sum passes full scale");
    check(m.atFullScale == 0, std::string(tag) + ": no sample of the published mix at full scale");
    check(mWhole.atFullScale == 0,
          std::string(tag) + ": nor anywhere in the run, the AGC's attack included");
    check(db(m.rms) > db(p.rms) - 3.0,
          std::string(tag) + ": the mix keeps its level (rms within 3 dB of the plain sum)");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    oneSliceUntouched();
    twoSlices(60.0, false);
    twoSlices(70.0, true);
    // The top of the AGC-T map at the reference LNA gain is the 90 dB case.
    const double top = Hl2DbReference{}.agcCeilingDb(100);
    check(top == 90.0, "AGC-T 100 maps to a 90 dB ceiling at the reference LNA gain");
    twoSlices(top, true);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
