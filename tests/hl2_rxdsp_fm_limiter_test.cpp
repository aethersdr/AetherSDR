// HL2 FM receive level, heard through the real Hl2RxDsp (#7222).
//
// FM bypasses the RX AGC and the detector output scales with deviation, so on
// open-squelch noise it reaches several times full scale unless WDSP's FM
// detector limiter runs. This pins that it does on the HL2, whether FM is
// built, entered live, or left and re-entered live, that a full-deviation tone
// keeps its level and shape, and that the limiter flag leaves USB and AM output
// identical.
//
// Offline and sample-counted: blockForOutput, no audio device, no wall clock.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples
#include "core/dsp/WdspChannel.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

using namespace AetherSDR;
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
constexpr int kIqRate = 48000;
constexpr int kAudioRate = 24000;
constexpr double kDeviationHz = 5000.0;   // RXA.c's default and the HL2's
constexpr int kSettleSeconds = 2;
constexpr int kMeasureSeconds = 2;

Hl2RxDsp::Config config(WdspChannel::Mode mode)
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kIqRate;
    cfg.audioSampleRateHz = kAudioRate;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.mode = mode;
    // Hl2Backend's FM passband (Hl2Backend.cpp, defaultFilterFor "FM").
    cfg.filterLowHz = mode == WdspChannel::Mode::Fm ? -8000.0 : 150.0;
    cfg.filterHighHz = mode == WdspChannel::Mode::Fm ? 8000.0 : 2850.0;
    cfg.blockForOutput = true;
    return cfg;
}

// What one stimulus produced: the left channel of the interleaved output.
struct Capture {
    std::vector<float> left;
    double peak() const
    {
        double p = 0.0;
        for (float s : left)
            p = std::max(p, static_cast<double>(std::fabs(s)));
        return p;
    }
    double rms() const
    {
        double sum = 0.0;
        for (float s : left)
            sum += static_cast<double>(s) * s;
        return left.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(left.size()));
    }
};

enum class Stimulus { Noise, FmTone };

struct Rig {
    Hl2RxDsp dsp;
    std::mt19937 rng{7222};
    std::normal_distribution<float> gauss{0.0f, 1.0f};
    Capture capture;
    bool collecting = false;
    long clock = 0;
    double fmPhase = 0.0;

    explicit Rig(WdspChannel::Mode mode)
    {
        std::string err;
        const bool ok = dsp.configure(config(mode), &err);
        check(ok, ok ? "Hl2RxDsp configures" : err.c_str());
        QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                         [this](const std::vector<float>& pcm) {
            if (!collecting)
                return;
            for (std::size_t i = 0; i + 1 < pcm.size(); i += 2)
                capture.left.push_back(pcm[i]);
        });
    }

    // Noise: complex Gaussian at ~-60 dBFS, no carrier (an empty channel).
    // FmTone: a -40 dBFS carrier at the tuned frequency, FM by toneHz at
    // kDeviationHz peak, with a faint noise floor.
    void feed(int seconds, Stimulus stimulus, double toneHz)
    {
        const std::size_t total = static_cast<std::size_t>(seconds) * kIqRate;
        const float noiseRms = stimulus == Stimulus::Noise ? 1.0e-3f : 1.0e-6f;
        const double carrier = stimulus == Stimulus::FmTone ? 1.0e-2 : 0.0;
        for (std::size_t off = 0; off < total; off += kEp6BlockSamples) {
            const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, total - off);
            std::vector<std::complex<float>> blk(n);
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(clock++) / kIqRate;
                const double inst = kDeviationHz * std::cos(2.0 * kPi * toneHz * t);
                fmPhase += 2.0 * kPi * inst / kIqRate;
                blk[i] = std::complex<float>(
                    static_cast<float>(carrier * std::cos(fmPhase)) + noiseRms * gauss(rng),
                    static_cast<float>(carrier * std::sin(fmPhase)) + noiseRms * gauss(rng));
            }
            dsp.processIqBlock(blk);
        }
    }

    Capture measure(Stimulus stimulus, double toneHz = 1000.0)
    {
        collecting = false;
        feed(kSettleSeconds, stimulus, toneHz);
        capture.left.clear();
        collecting = true;
        feed(kMeasureSeconds, stimulus, toneHz);
        collecting = false;
        return capture;
    }
};

// Fundamental amplitude by least squares on sin/cos/DC, and THD+N as the
// residual RMS over the fundamental's RMS.
struct ToneFit {
    double amplitude = 0.0;
    double thdN = 1.0;
};
ToneFit fitTone(const std::vector<float>& x, double toneHz)
{
    double ss = 0, cc = 0, sx = 0, cx = 0, x1 = 0;
    const double n = static_cast<double>(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double w = 2.0 * kPi * toneHz * static_cast<double>(i) / kAudioRate;
        const double s = std::sin(w), c = std::cos(w);
        ss += s * s; cc += c * c; sx += s * x[i]; cx += c * x[i]; x1 += x[i];
    }
    // Over a whole number of cycles sin, cos and 1 are orthogonal to ~1e-12.
    const double a = sx / ss, b = cx / cc, dc = x1 / n;
    double res = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double w = 2.0 * kPi * toneHz * static_cast<double>(i) / kAudioRate;
        const double e = x[i] - (a * std::sin(w) + b * std::cos(w) + dc);
        res += e * e;
    }
    ToneFit fit;
    fit.amplitude = std::hypot(a, b);
    const double fundRms = fit.amplitude / std::sqrt(2.0);
    fit.thdN = fundRms > 0.0 ? std::sqrt(res / n) / fundRms : 1.0;
    return fit;
}

// Two bare channels, identical but for fmLimiter, fed the same IQ: outside FM
// the flag must not change a sample.
double limiterFlagDifference(WdspChannel::Mode mode)
{
    WdspChannel::Config c;
    c.direction = WdspChannel::Direction::Receive;
    c.inputBlockSize = 1024;
    c.dspBlockSize = 1024;
    c.inputSampleRate = kIqRate;
    c.dspSampleRate = 48000;
    c.outputSampleRate = kAudioRate;
    c.mode = mode;
    c.filterLowHz = mode == WdspChannel::Mode::Am ? -4000.0 : 150.0;
    c.filterHighHz = mode == WdspChannel::Mode::Am ? 4000.0 : 2850.0;
    c.rxPanelGain = Hl2RxDsp::kRxPanelGain;
    c.blockForOutput = true;
    WdspChannel::Config limited = c;
    limited.fmLimiter = true;
    std::string err;
    auto plain = WdspChannel::create(c, &err);
    auto lim = WdspChannel::create(limited, &err);
    if (!plain || !lim) {
        check(false, err.c_str());
        return 1.0;
    }
    std::mt19937 rng{42};
    std::normal_distribution<float> gauss{0.0f, 1.0e-3f};
    std::vector<float> i(c.inputBlockSize), q(c.inputBlockSize);
    std::vector<float> l1(plain->outputBlockSize()), r1(l1.size());
    std::vector<float> l2(lim->outputBlockSize()), r2(l2.size());
    double maxDiff = 0.0;
    long clock = 0;
    for (int block = 0; block < 96; ++block) {
        for (std::size_t k = 0; k < i.size(); ++k) {
            const double w = 2.0 * kPi * 1000.0 * static_cast<double>(clock++) / kIqRate;
            i[k] = static_cast<float>(1.0e-2 * std::cos(w)) + gauss(rng);
            q[k] = static_cast<float>(-1.0e-2 * std::sin(w)) + gauss(rng);
        }
        if (plain->processIq(i, q, l1, r1) != WdspChannel::ProcessResult::Ok
            || lim->processIq(i, q, l2, r2) != WdspChannel::ProcessResult::Ok) {
            check(false, "processIq returned an error");
            return 1.0;
        }
        for (std::size_t k = 0; k < l1.size(); ++k)
            maxDiff = std::max({maxDiff, static_cast<double>(std::fabs(l1[k] - l2[k])),
                                static_cast<double>(std::fabs(r1[k] - r2[k]))});
    }
    return maxDiff;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // 1. Open-squelch noise, FM built at configure.
    {
        Rig rig(WdspChannel::Mode::Fm);
        const Capture noise = rig.measure(Stimulus::Noise);
        std::printf("FM built, noise: peak %.4f rms %.4f (%zu samples)\n",
                    noise.peak(), noise.rms(), noise.left.size());
        check(noise.left.size() > static_cast<std::size_t>(kAudioRate),
              "FM noise produced audio");
        check(noise.peak() <= 1.0, "FM built: noise peak is within full scale");
    }

    // 2. Same, FM entered live from USB without a rebuild (Hl2RxDsp::setMode).
    {
        Rig rig(WdspChannel::Mode::Usb);
        rig.dsp.setFilter(-8000.0, 8000.0);
        rig.dsp.setMode(WdspChannel::Mode::Fm);
        const Capture noise = rig.measure(Stimulus::Noise);
        std::printf("FM entered live, noise: peak %.4f rms %.4f\n", noise.peak(), noise.rms());
        check(noise.peak() <= 1.0, "FM entered live: noise peak is within full scale");
    }

    // 2b. USB -> FM -> USB -> FM without a rebuild (case from #7235): leaving
    // FM and coming back must leave the limiter running on the second entry.
    {
        Rig rig(WdspChannel::Mode::Usb);
        rig.dsp.setFilter(-8000.0, 8000.0);
        rig.dsp.setMode(WdspChannel::Mode::Fm);
        const Capture first = rig.measure(Stimulus::Noise);
        rig.dsp.setFilter(150.0, 2850.0);
        rig.dsp.setMode(WdspChannel::Mode::Usb);
        rig.feed(1, Stimulus::Noise, 1000.0);
        rig.dsp.setFilter(-8000.0, 8000.0);
        rig.dsp.setMode(WdspChannel::Mode::Fm);
        const Capture second = rig.measure(Stimulus::Noise);
        std::printf("USB->FM->USB->FM, noise: first FM peak %.4f, second FM peak %.4f (%zu samples)\n",
                    first.peak(), second.peak(), second.left.size());
        check(second.left.size() > static_cast<std::size_t>(kAudioRate),
              "FM re-entered: noise produced audio");
        check(first.peak() <= 1.0, "USB->FM: noise peak is within full scale");
        check(second.peak() <= 1.0, "USB->FM->USB->FM: noise peak is within full scale");
    }

    // 3. A full-deviation 1 kHz tone keeps a usable level and its shape. The
    // limiter's pre-gain is 0.4 and its target 0.9, so a tone below the
    // target passes linearly.
    {
        Rig rig(WdspChannel::Mode::Fm);
        const Capture tone = rig.measure(Stimulus::FmTone, 1000.0);
        const ToneFit fit = fitTone(tone.left, 1000.0);
        std::printf("FM tone 1 kHz at 5 kHz deviation: peak %.4f amplitude %.4f THD+N %.4f%%\n",
                    tone.peak(), fit.amplitude, 100.0 * fit.thdN);
        check(tone.peak() <= 1.0, "full-deviation tone peak is within full scale");
        check(fit.amplitude >= 0.3 && fit.amplitude <= 0.95,
              "full-deviation tone amplitude is in [0.3, 0.95]");
        check(fit.thdN < 0.01, "full-deviation tone THD+N is under 1 %");
    }

    // 4. Informational: a 400 Hz tone, where de-emphasis gives the most gain.
    {
        Rig rig(WdspChannel::Mode::Fm);
        const Capture tone = rig.measure(Stimulus::FmTone, 400.0);
        const ToneFit fit = fitTone(tone.left, 400.0);
        std::printf("FM tone 400 Hz at 5 kHz deviation: peak %.4f amplitude %.4f THD+N %.4f%%\n",
                    tone.peak(), fit.amplitude, 100.0 * fit.thdN);
        check(tone.peak() <= 1.0, "400 Hz full-deviation tone peak is within full scale");
    }

    // 5. Outside FM the limiter flag changes nothing.
    {
        const double usb = limiterFlagDifference(WdspChannel::Mode::Usb);
        const double am = limiterFlagDifference(WdspChannel::Mode::Am);
        std::printf("fmLimiter on vs off: USB max diff %.3g, AM max diff %.3g\n", usb, am);
        check(usb < 1.0e-6, "USB output is unchanged by the FM limiter flag");
        check(am < 1.0e-6, "AM output is unchanged by the FM limiter flag");
    }

    if (g_failures == 0)
        std::fprintf(stdout, "hl2_rxdsp_fm_limiter_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
