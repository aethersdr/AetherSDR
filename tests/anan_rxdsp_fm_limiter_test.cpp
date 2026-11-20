// ANAN FM receive level, heard through the real AnanRxDsp (#7252).
//
// FM bypasses the RX AGC and the detector output scales with deviation, so on
// open-squelch noise it reaches several times full scale unless WDSP's FM
// limiter runs, and the RXA panel after it is at unity. The ANAN keeps WDSP's
// panel gain of 4.0 outside FM, so this also pins that the gain moves with the
// mode: FM built, entered live, entered during a rebuild, and left again with
// the USB level back where it was. Mirrors hl2_rxdsp_fm_limiter_test.
//
// Offline and sample-counted: blockForOutput, no audio device, no wall clock.

#include "core/backends/anan/AnanRxDsp.h"
#include "core/dsp/WdspChannel.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::anan;

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
constexpr std::size_t kFeedBlock = 1024;
constexpr double kDeviationHz = 5000.0;   // RXA.c's default
constexpr int kSettleSeconds = 2;
constexpr int kMeasureSeconds = 2;

// AnanBackend's passbands (AnanBackend.cpp, defaultFilterFor "FM" / "USB").
constexpr double kFmLowHz = -8000.0, kFmHighHz = 8000.0;
constexpr double kUsbLowHz = 150.0, kUsbHighHz = 2850.0;

AnanRxDsp::Config config(WdspChannel::Mode mode)
{
    AnanRxDsp::Config cfg;
    cfg.inputSampleRateHz = kIqRate;
    cfg.audioSampleRateHz = kAudioRate;
    cfg.dspBlockSize = 1024;
    cfg.mode = mode;
    cfg.filterLowHz = mode == WdspChannel::Mode::Fm ? kFmLowHz : kUsbLowHz;
    cfg.filterHighHz = mode == WdspChannel::Mode::Fm ? kFmHighHz : kUsbHighHz;
    cfg.blockForOutput = true;
    return cfg;
}

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
    AnanRxDsp dsp;
    std::mt19937 rng{7252};
    std::normal_distribution<float> gauss{0.0f, 1.0f};
    Capture capture;
    bool collecting = false;
    long clock = 0;
    double fmPhase = 0.0;

    explicit Rig(WdspChannel::Mode mode)
    {
        std::string err;
        const bool ok = dsp.configure(config(mode), &err);
        check(ok, ok ? "AnanRxDsp configures" : err.c_str());
        QObject::connect(&dsp, &AnanRxDsp::audioReady, &dsp,
                         [this](const std::vector<float>& pcm) {
            if (!collecting)
                return;
            for (std::size_t i = 0; i + 1 < pcm.size(); i += 2)
                capture.left.push_back(pcm[i]);
        });
    }

    void enter(WdspChannel::Mode mode)
    {
        const bool fm = mode == WdspChannel::Mode::Fm;
        dsp.setFilter(fm ? kFmLowHz : kUsbLowHz, fm ? kFmHighHz : kUsbHighHz);
        dsp.setMode(mode);
    }

    // Noise: complex Gaussian at ~-60 dBFS, no carrier (an empty channel).
    // FmTone: a -40 dBFS carrier at the tuned frequency, FM by toneHz at
    // kDeviationHz peak, with a faint noise floor.
    void feed(int seconds, Stimulus stimulus, double toneHz)
    {
        const std::size_t total = static_cast<std::size_t>(seconds) * kIqRate;
        const float noiseRms = stimulus == Stimulus::Noise ? 1.0e-3f : 1.0e-6f;
        const double carrier = stimulus == Stimulus::FmTone ? 1.0e-2 : 0.0;
        for (std::size_t off = 0; off < total; off += kFeedBlock) {
            const std::size_t n = std::min(kFeedBlock, total - off);
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

// Two bare channels fed the same IQ: the ANAN channel as it was before #7252
// (neither field set) and as it is now. Outside FM no sample may change.
double fmFieldsDifference(WdspChannel::Mode mode)
{
    WdspChannel::Config c;
    c.direction = WdspChannel::Direction::Receive;
    c.inputBlockSize = 1024;
    c.dspBlockSize = 1024;
    c.inputSampleRate = kIqRate;
    c.dspSampleRate = AnanRxDsp::kWdspDspSampleRateHz;
    c.outputSampleRate = kAudioRate;
    c.mode = mode;
    c.filterLowHz = mode == WdspChannel::Mode::Am ? -4000.0 : kUsbLowHz;
    c.filterHighHz = mode == WdspChannel::Mode::Am ? 4000.0 : kUsbHighHz;
    c.blockForOutput = true;
    WdspChannel::Config now = c;
    now.fmLimiter = true;
    now.fmRxPanelGain = AnanRxDsp::kFmRxPanelGain;
    std::string err;
    auto before = WdspChannel::create(c, &err);
    auto after = WdspChannel::create(now, &err);
    if (!before || !after) {
        check(false, err.c_str());
        return 1.0;
    }
    std::mt19937 rng{42};
    std::normal_distribution<float> gauss{0.0f, 1.0e-3f};
    std::vector<float> i(c.inputBlockSize), q(c.inputBlockSize);
    std::vector<float> l1(before->outputBlockSize()), r1(l1.size());
    std::vector<float> l2(after->outputBlockSize()), r2(l2.size());
    double maxDiff = 0.0;
    long clock = 0;
    for (int block = 0; block < 96; ++block) {
        for (std::size_t k = 0; k < i.size(); ++k) {
            const double w = 2.0 * kPi * 1000.0 * static_cast<double>(clock++) / kIqRate;
            i[k] = static_cast<float>(1.0e-2 * std::cos(w)) + gauss(rng);
            q[k] = static_cast<float>(-1.0e-2 * std::sin(w)) + gauss(rng);
        }
        if (before->processIq(i, q, l1, r1) != WdspChannel::ProcessResult::Ok
            || after->processIq(i, q, l2, r2) != WdspChannel::ProcessResult::Ok) {
            check(false, "processIq returned an error");
            return 1.0;
        }
        for (std::size_t k = 0; k < l1.size(); ++k)
            maxDiff = std::max({maxDiff, static_cast<double>(std::fabs(l1[k] - l2[k])),
                                static_cast<double>(std::fabs(r1[k] - r2[k]))});
    }
    return maxDiff;
}

double db(double ratio) { return 20.0 * std::log10(ratio); }

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

    // 2. FM entered live from USB without a rebuild, left, and entered again;
    // the USB level after leaving FM must be the USB level before it.
    {
        Rig rig(WdspChannel::Mode::Usb);
        const Capture usbBefore = rig.measure(Stimulus::Noise);
        rig.enter(WdspChannel::Mode::Fm);
        const Capture first = rig.measure(Stimulus::Noise);
        rig.enter(WdspChannel::Mode::Usb);
        const Capture usbAfter = rig.measure(Stimulus::Noise);
        rig.enter(WdspChannel::Mode::Fm);
        const Capture second = rig.measure(Stimulus::Noise);
        const double usbShiftDb = db(usbAfter.rms() / usbBefore.rms());
        std::printf("USB->FM->USB->FM, noise: USB rms %.4f, FM peak %.4f, USB rms %.4f "
                    "(%+.2f dB), FM peak %.4f\n",
                    usbBefore.rms(), first.peak(), usbAfter.rms(), usbShiftDb,
                    second.peak());
        check(second.left.size() > static_cast<std::size_t>(kAudioRate),
              "FM re-entered: noise produced audio");
        check(first.peak() <= 1.0, "USB->FM live: noise peak is within full scale");
        check(second.peak() <= 1.0, "USB->FM->USB->FM live: noise peak is within full scale");
        check(std::fabs(usbShiftDb) < 1.0,
              "USB level after leaving FM is within 1 dB of the level before it");
    }

    // 3. FM chosen while a rebuild is in flight: setMode() only records it, and
    // installRebuiltChannel() must bring the panel gain along with the mode.
    {
        Rig rig(WdspChannel::Mode::Usb);
        rig.dsp.beginRebuild();
        rig.enter(WdspChannel::Mode::Fm);
        auto built = AnanRxDsp::buildChannel(config(WdspChannel::Mode::Usb));
        check(rig.dsp.installRebuiltChannel(std::move(built)), "rebuilt channel installs");
        const Capture noise = rig.measure(Stimulus::Noise);
        std::printf("FM entered during a rebuild, noise: peak %.4f rms %.4f\n",
                    noise.peak(), noise.rms());
        check(noise.peak() <= 1.0, "FM entered during a rebuild: noise peak is within full scale");
    }

    // 4. A full-deviation 1 kHz tone keeps a usable level and its shape.
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

    // 5. A 400 Hz tone, where de-emphasis gives the most gain.
    {
        Rig rig(WdspChannel::Mode::Fm);
        const Capture tone = rig.measure(Stimulus::FmTone, 400.0);
        const ToneFit fit = fitTone(tone.left, 400.0);
        std::printf("FM tone 400 Hz at 5 kHz deviation: peak %.4f amplitude %.4f THD+N %.4f%%\n",
                    tone.peak(), fit.amplitude, 100.0 * fit.thdN);
        check(tone.peak() <= 1.0, "400 Hz full-deviation tone peak is within full scale");
    }

    // 6. Outside FM the two fields change nothing, the panel's 4.0 included.
    {
        const double usb = fmFieldsDifference(WdspChannel::Mode::Usb);
        const double am = fmFieldsDifference(WdspChannel::Mode::Am);
        std::printf("FM fields on vs off: USB max diff %.3g, AM max diff %.3g\n", usb, am);
        check(usb < 1.0e-6, "USB output is unchanged by the FM fields");
        check(am < 1.0e-6, "AM output is unchanged by the FM fields");
    }

    // 7. The channel refuses a non-finite or negative FM panel gain, as it
    // does rxPanelGain.
    {
        WdspChannel::Config bad;
        bad.fmRxPanelGain = -1.0;
        std::string err;
        check(!WdspChannel::create(bad, &err), "a negative fmRxPanelGain is refused");
        bad.fmRxPanelGain = std::nan("");
        check(!WdspChannel::create(bad, &err), "a NaN fmRxPanelGain is refused");
        check(err.find("fmRxPanelGain") != std::string::npos, "the refusal names fmRxPanelGain");
        WdspChannel::Config badPanel;
        badPanel.rxPanelGain = -1.0;
        err.clear();
        check(!WdspChannel::create(badPanel, &err)
                  && err.find("rxPanelGain") != std::string::npos
                  && err.find("fmRxPanelGain") == std::string::npos,
              "a negative rxPanelGain is refused, and the refusal names rxPanelGain");
    }

    if (g_failures == 0)
        std::fprintf(stdout, "anan_rxdsp_fm_limiter_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
