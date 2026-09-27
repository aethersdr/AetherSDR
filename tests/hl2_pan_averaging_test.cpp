// #5678 row 2.1 on the Hermes-Lite 2: the operator's FFT AVG reaches the
// panadapter, as a TIME CONSTANT, and the average is in the domain the
// weighted toggle selects (power by default). RFC #5782's ruling puts the
// averaging in the host's backend layer; this is that layer's arithmetic.
//
// What is asserted, and why each needs a control:
//   1. the 0..100 -> ms mapping and the blend weight (pure arithmetic);
//   2. on noise, the average cuts the per-bin power variance by the factor an
//      exponential average of independent exponential variates predicts,
//      alpha / (2 - alpha) -- with the un-averaged spectrum as the positive
//      control that the fixture's variance is what theory says it is (1);
//   3. the response to a step takes the SAME wall time at a 21 ms and a
//      100 ms display interval -- the fps-slider coupling on8st raised on
//      #5782 -- with a frame-depth estimator as the control that the fixture
//      can see the coupling at all;
//   4. the weighted toggle really changes the domain (log-recursive reads the
//      noise floor ~2.5 dB lower, E[ln X] = ln E[X] - gamma);
//   5. dropAverage() forgets the old axis and a replayed setting does not;
//   6. Hl2RxDsp re-applies the operator's averaging to the fresh Hl2Spectrum
//      every rebuild constructs -- without it the first zoom turns it off.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/Hl2Spectrum.h"

#include <QCoreApplication>

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
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

constexpr int kFft = 1024;
constexpr double kFs = 48000.0;
constexpr int kBlock = 126;   // one EP6 block, as Hl2RxDsp is fed

struct Noise {
    std::mt19937 rng {0x5678u};
    std::normal_distribution<float> n {0.0f, 1.0f};
    std::vector<std::complex<float>> block(int len, float sigma)
    {
        std::vector<std::complex<float>> v(static_cast<std::size_t>(len));
        for (auto& s : v)
            s = {sigma * n(rng), sigma * n(rng)};
        return v;
    }
};

// Bins that measure the noise: skip the few around DC, where the per-frame
// DC removal takes a degree of freedom out.
bool usable(int k) { return std::abs(k - kFft / 2) > 3; }

// Mean over usable bins of the linear power, and the squared coefficient of
// variation across them (variance / mean^2).
void powerStats(const std::vector<float>& bins, double& mean, double& cv2)
{
    double s = 0.0, s2 = 0.0;
    int n = 0;
    for (int k = 0; k < kFft; ++k) {
        if (!usable(k))
            continue;
        const double p = std::pow(10.0, bins[static_cast<std::size_t>(k)] / 10.0);
        s += p;
        s2 += p * p;
        ++n;
    }
    mean = s / n;
    cv2 = (s2 / n - mean * mean) / (mean * mean);
}

double meanDb(const std::vector<float>& bins)
{
    double s = 0.0;
    int n = 0;
    for (int k = 0; k < kFft; ++k)
        if (usable(k)) { s += bins[static_cast<std::size_t>(k)]; ++n; }
    return s / n;
}

// Uncapped: every kFft samples is a frame. Returns the mean CV^2 over
// `snapshots` frames spaced `spacing` apart, after `warmup` frames.
double measuredCv2(double tauMs, bool logDomain, double* meanDbOut = nullptr)
{
    Hl2Spectrum spec(kFft, kFs);
    spec.setAverageTimeMs(tauMs);
    spec.setLogAverage(logDomain);
    Noise noise;
    std::vector<float> bins;
    const int warmup = 80, snapshots = 40, spacing = 25;
    double acc = 0.0, accDb = 0.0;
    int taken = 0;
    for (int f = 0; f < warmup + snapshots * spacing; ++f) {
        spec.process(noise.block(kFft, 0.01f), bins);
        if (f >= warmup && (f - warmup) % spacing == 0) {
            double m = 0.0, c = 0.0;
            powerStats(bins, m, c);
            acc += c;
            accDb += meanDb(bins);
            ++taken;
        }
    }
    if (meanDbOut)
        *meanDbOut = accDb / taken;
    return acc / taken;
}

// Emulates Hl2RxDsp's shaper: EP6-sized blocks; process() only while a frame
// is due (every `intervalSamples`), accumulate() otherwise. A power step of
// x10 lands at `stepAt`. Returns the time in ms from the step to the first
// emitted frame whose mean power has covered 63.2 % of the step -- which is
// the time constant, for a true exponential average.
double stepResponseMs(int intervalSamples, double tauMs, int depthFrames)
{
    Hl2Spectrum spec(kFft, kFs);
    if (depthFrames > 0)
        spec.setAverageFrames(depthFrames);
    else
        spec.setAverageTimeMs(tauMs);
    Noise noise;
    std::vector<float> bins;
    const long stepAt = static_cast<long>(4.0 * kFs);
    const long end = static_cast<long>(16.0 * kFs);
    const float lo = 0.01f, hi = 0.01f * std::sqrt(10.0f);
    long t = 0, lastEmit = -intervalSamples;
    double before = 0.0;
    int beforeN = 0;
    while (t < end) {
        const float sigma = t >= stepAt ? hi : lo;
        const auto blk = noise.block(kBlock, sigma);
        t += kBlock;
        if (t - lastEmit >= intervalSamples) {
            if (spec.process(blk, bins) > 0) {
                lastEmit = t;
                double m = 0.0, c = 0.0;
                powerStats(bins, m, c);
                if (t < stepAt && t > stepAt - static_cast<long>(kFs)) {
                    before += m;
                    ++beforeN;
                } else if (t >= stepAt && beforeN > 0) {
                    const double p0 = before / beforeN;
                    if (m >= p0 + 0.632 * (10.0 * p0 - p0))
                        return 1000.0 * static_cast<double>(t - stepAt) / kFs;
                }
            }
        } else {
            spec.accumulate(blk);
        }
    }
    return 1e9;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. mapping ------------------------------------------------------
    static_assert(Hl2Backend::kMsPerAverageStep == 10,
                  "one FFT AVG step is 10 ms, the ANAN/deskHPSDR unit");
    static_assert(Hl2Backend::averageTimeMsForStep(0) == 0, "0 = no averaging");
    static_assert(Hl2Backend::averageTimeMsForStep(1) == 10, "1 step = 10 ms");
    static_assert(Hl2Backend::averageTimeMsForStep(50) == 500, "50 = 0.5 s");
    static_assert(Hl2Backend::averageTimeMsForStep(100) == 1000, "100 = 1 s");
    static_assert(Hl2Backend::averageTimeMsForStep(-7) == 0, "below range clamps to off");
    static_assert(Hl2Backend::averageTimeMsForStep(250) == 1000, "above range clamps to 1 s");

    check(Hl2Spectrum::blendAlpha(0.02, 0.0) == 1.0, "tau 0: the new frame replaces the state");
    check(std::abs(Hl2Spectrum::blendAlpha(0.3, 0.3) - (1.0 - std::exp(-1.0))) < 1e-12,
          "dt = tau: alpha = 1 - 1/e");
    {
        // The property that makes it a TIME: two blends of dt/2 leave exactly
        // the history one blend of dt leaves, so the cadence cannot matter.
        const double a1 = Hl2Spectrum::blendAlpha(0.05, 0.4);
        const double a2 = Hl2Spectrum::blendAlpha(0.10, 0.4);
        check(std::abs((1.0 - a1) * (1.0 - a1) - (1.0 - a2)) < 1e-12,
              "history weight composes: two half-intervals == one interval");
    }

    // ---- 2. variance on noise --------------------------------------------
    {
        const double raw = measuredCv2(0.0, false);
        std::printf("raw per-bin power CV^2 = %.3f (theory 1.000)\n", raw);
        check(raw > 0.85 && raw < 1.15,
              "control: un-averaged per-bin noise power is exponential (CV^2 ~ 1)");

        for (const double tauMs : {100.0, 300.0}) {
            const double a = Hl2Spectrum::blendAlpha(kFft / kFs, tauMs / 1000.0);
            const double expected = a / (2.0 - a);
            const double got = measuredCv2(tauMs, false);
            std::printf("tau %.0f ms: CV^2 = %.4f, expected alpha/(2-alpha) = %.4f (ratio %.3f)\n",
                        tauMs, got, expected, got / expected);
            check(got / expected > 0.85 && got / expected < 1.15,
                  "averaged per-bin variance falls by alpha/(2-alpha)");
        }
    }

    // ---- 3. the time constant does not move with the display rate --------
    {
        const double tauMs = 500.0;
        const double fast = stepResponseMs(kFft, tauMs, 0);          // ~21 ms frames
        const double slow = stepResponseMs(4800, tauMs, 0);          // 100 ms frames
        std::printf("step 63%%: %.0f ms at 21 ms frames, %.0f ms at 100 ms frames (tau %.0f)\n",
                    fast, slow, tauMs);
        // The bound is +-(frame interval + FFT window + noise margin), and
        // symmetric on purpose. Each displayed periodogram stands for the
        // whole dt since the previous frame, so a step landing mid-interval
        // is credited up to one interval EARLY by the frame that first sees
        // it, and the crossing is only observable at the next frame, up to
        // one interval LATE. Measured at 100 ms: 426 ms, the early side.
        // What must NOT happen is the control's 4x: the time constant stays
        // tau, quantised to the display interval.
        const double win = 1000.0 * kFft / kFs;
        check(std::abs(fast - tauMs) < win + win + 30.0,
              "63% of a step after ~tau at the uncapped frame rate");
        check(std::abs(slow - tauMs) < 100.0 + win + 30.0,
              "63% of a step after ~tau at a 100 ms display interval too");

        // Control: a DEPTH tuned to tau at the fast rate (tau / 21.3 ms
        // frames) is what the fps slider would have moved.
        const int depth = static_cast<int>(std::lround(tauMs / (1000.0 * kFft / kFs)));
        const double depthFast = stepResponseMs(kFft, 0.0, depth);
        const double depthSlow = stepResponseMs(4800, 0.0, depth);
        std::printf("control, depth %d frames: %.0f ms fast, %.0f ms slow\n",
                    depth, depthFast, depthSlow);
        check(depthSlow > 3.0 * depthFast,
              "control: a frame-count depth's response time scales with the frame interval");
    }

    // ---- 4. the toggle is the domain -------------------------------------
    {
        double powerDb = 0.0, logDb = 0.0;
        measuredCv2(300.0, false, &powerDb);
        measuredCv2(300.0, true, &logDb);
        std::printf("mean floor: power %.2f dBFS, log-recursive %.2f dBFS (diff %.2f)\n",
                    powerDb, logDb, logDb - powerDb);
        // gamma * 10/ln10 = 2.51 dB, less the small Jensen bias the power
        // average itself carries through the final log.
        check(logDb - powerDb < -1.9 && logDb - powerDb > -2.9,
              "log-recursive reads the noise floor ~2.5 dB below the power average");
    }

    // ---- 5. retune drop, and a replay that must not drop ------------------
    {
        auto tone = [](int k0, float amp) {
            std::vector<std::complex<float>> v(kFft);
            for (int n = 0; n < kFft; ++n) {
                const double ph = 2.0 * 3.14159265358979323846 * k0 * n / kFft;
                v[static_cast<std::size_t>(n)] = amp * std::complex<float>(
                    static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
            }
            return v;
        };
        const std::size_t oldBin = (100 + kFft / 2) % kFft;
        const auto oldTone = tone(100, 0.5f);
        const auto newTone = tone(-200, 0.5f);

        Hl2Spectrum ref(kFft, kFs);   // averaging off: the raw frame
        std::vector<float> raw;
        ref.process(newTone, raw);

        std::vector<float> bins;
        Hl2Spectrum kept(kFft, kFs);
        kept.setAverageTimeMs(500.0);
        for (int i = 0; i < 20; ++i) kept.process(oldTone, bins);
        kept.setAverageTimeMs(500.0);   // replay of the same value
        kept.process(newTone, bins);
        check(bins[oldBin] > raw[oldBin] + 40.0f,
              "control: without a drop the old axis ghosts into the next frame, and a "
              "replayed setting keeps the average");

        Hl2Spectrum dropped(kFft, kFs);
        dropped.setAverageTimeMs(500.0);
        for (int i = 0; i < 20; ++i) dropped.process(oldTone, bins);
        dropped.dropAverage();
        dropped.process(newTone, bins);
        check(bins == raw, "after dropAverage() the next frame is the raw frame, bit for bit");
    }

    // ---- 6. the operator's averaging survives a rebuild ------------------
    {
        Hl2RxDsp dsp;
        Hl2RxDsp::Config cfg;
        cfg.inputSampleRateHz = 48000;
        cfg.audioSampleRateHz = 48000;
        cfg.dspBlockSize = 1024;
        cfg.fftSize = 256;
        cfg.blockForOutput = true;
        std::string err;
        check(dsp.configure(cfg, &err), "configure");
        check(dsp.spectrumAverageMsApplied() == 0.0, "control: a fresh chain does not average");
        dsp.setSpectrumAverageMs(Hl2Backend::averageTimeMsForStep(30));
        dsp.setSpectrumLogAverage(true);
        check(dsp.spectrumAverageMsApplied() == 300.0, "applied to the live spectrum");

        cfg.inputSampleRateHz = 96000;   // a zoom: new geometry, new Hl2Spectrum
        cfg.fftSize = 512;
        check(dsp.configure(cfg, &err), "reconfigure");
        check(dsp.spectrumAverageMsApplied() == 300.0,
              "the averaging time survives the rebuild");
        check(dsp.spectrumLogAverageApplied(), "and so does the domain");
    }

    if (g_failures == 0)
        std::printf("hl2_pan_averaging_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
