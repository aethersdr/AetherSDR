// WdspChannel noise-blanker hold: releasing the hold must NOT flush the stage.
//
// The blanker fires on |x| > threshold * running average. A flush resets that
// average to full scale, leaving the stage blind for ~200 ms at backtau 0.05 s,
// so "not flushed" is observable as "still blanks impulses right after release".
// Each arm is differential against a blanker-off channel fed the same stimulus.

#include "core/dsp/WdspChannel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

constexpr int kFs = 48000;
constexpr std::size_t kBlock = 256;
constexpr double kToneAmp = 0.02;
constexpr double kImpulseAmp = 0.5;
// Trigger ~7.6x the average: well above the tone, well below the 25x impulse.
constexpr int kNbLevel = 80;
constexpr std::size_t kImpulsePeriod = 2000;

enum class Release { Hold, Reenable };

// Arm on a clean tone, then "transmit": zeros for 0.5 s with the hold set (the
// stage skips them). Then receive returns WITH impulses from the first sample.
// Release::Reenable ends TX with disable+enable instead of releasing the hold,
// which flushes: the control that proves this observable can see a flush.
// Returns the demodulated audio peak over the first 200 ms after the edge.
double postEdgePeak(bool blankerOn, Release release)
{
    WdspChannel::Config cfg;
    cfg.inputBlockSize = kBlock;
    cfg.dspBlockSize = kBlock;
    cfg.mode = WdspChannel::Mode::Cwu;   // linear phase: impulse peaks are clean
    cfg.agcMode = 0;                     // an AGC would level the very peaks measured
    cfg.blockForOutput = true;
    std::string err;
    auto ch = WdspChannel::create(cfg, &err);
    if (!ch) {
        check(false, err.c_str());
        return -1.0;
    }
    if (blankerOn && !ch->setNoiseBlanker(true, kNbLevel)) {
        check(false, "setNoiseBlanker(true) accepted");
        return -1.0;
    }

    std::vector<float> i(kBlock), q(kBlock);
    std::vector<float> l(ch->outputBlockSize()), r(ch->outputBlockSize());
    std::size_t n = 0;
    const auto run = [&](std::size_t samples, bool tone, bool impulses, double* peak) {
        for (std::size_t done = 0; done < samples; done += kBlock, n += kBlock) {
            for (std::size_t k = 0; k < kBlock; ++k) {
                const double ph = 2.0 * std::numbers::pi * 1000.0 *
                                  static_cast<double>(n + k) / kFs;
                double re = tone ? kToneAmp * std::cos(ph) : 0.0;
                const double im = tone ? -kToneAmp * std::sin(ph) : 0.0;
                if (impulses && ((done + k) % kImpulsePeriod) == 0)
                    re += kImpulseAmp;
                i[k] = static_cast<float>(re);
                q[k] = static_cast<float>(im);
            }
            if (ch->processIq(i, q, l, r) != WdspChannel::ProcessResult::Ok) {
                check(false, "processIq returned Ok");
                return;
            }
            if (peak)
                for (float s : l)
                    *peak = std::max(*peak, static_cast<double>(std::abs(s)));
        }
    };

    run(kFs, true, false, nullptr);              // arm the running average
    ch->setNoiseBlankerHold(true);
    run(kFs / 2, false, false, nullptr);         // "transmit": silence, held
    ch->setNoiseBlankerHold(false);
    if (release == Release::Reenable && blankerOn)
        check(ch->setNoiseBlanker(false, kNbLevel) && ch->setNoiseBlanker(true, kNbLevel),
              "control arm: blanker disabled and re-enabled");
    double peak = 0.0;
    run(kFs / 5, true, true, &peak);             // receive, impulses from sample 0
    return peak;
}

}  // namespace

int main()
{
    const double off = postEdgePeak(false, Release::Hold);
    const double held = postEdgePeak(true, Release::Hold);
    const double reenabled = postEdgePeak(true, Release::Reenable);
    std::printf("  post-edge impulse peak: off=%.4f held=%.4f reenabled=%.4f\n",
                off, held, reenabled);

    check(off > 0.0 && held > 0.0 && reenabled > 0.0, "all three arms produced audio");
    // The flushed control is blind: if this fails the observable cannot see a
    // flush and the assertion below proves nothing.
    check(reenabled > off * 0.95,
          "control: a flushed blanker passes impulses in the first 200 ms");
    check(held < off * 0.75,
          "released hold keeps the running average: impulses are blanked at once");
    if (g_failures == 0)
        std::printf("wdsp_nb_hold_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
