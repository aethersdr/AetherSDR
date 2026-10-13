// The HL2 transmit ALC applies its per-block gain decision as a path inside the
// block, not as a step at the block's first sample (#5912).
//
// Everything here is sample-counted on the levelled audio the modulator
// receives: no modulator, no thread, no clock. Each measurement is also taken
// on a zero-order-hold model of the same decisions, as the positive control
// that shows the instrument sees a block-edge step when there is one.

#include "core/backends/hl2/Hl2TxAlc.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <random>
#include <span>
#include <vector>

using AetherSDR::hl2::Hl2TxAlc;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 24000.0;
constexpr std::size_t kBlock = 512;
constexpr double kTarget = 0.85;
constexpr double kReleaseSec = 0.500;

struct Run {
    std::vector<float> pre;          // input after mic gain
    std::vector<float> out;          // levelled
    std::vector<double> blockGain;   // gain() after each block
};

Hl2TxAlc::Settings settings()
{
    Hl2TxAlc::Settings s;
    s.enabled = true;
    s.targetPeak = kTarget;
    s.releaseSec = kReleaseSec;
    s.sampleRateHz = kFs;
    return s;
}

std::vector<float> preOf(const std::vector<float>& in, double micGain)
{
    std::vector<float> pre(in.size());
    for (std::size_t n = 0; n < in.size(); ++n)
        pre[n] = static_cast<float>(in[n] * micGain);
    return pre;
}

// The stage under test, block by block.
Run runAlc(const std::vector<float>& in, double micGain)
{
    Run r;
    r.pre = preOf(in, micGain);
    r.out.resize(in.size());
    Hl2TxAlc alc;
    const Hl2TxAlc::Settings s = settings();
    for (std::size_t off = 0; off + kBlock <= in.size(); off += kBlock) {
        alc.process(std::span<const float>(in.data() + off, kBlock), micGain, s,
                    std::span<float>(r.out.data() + off, kBlock));
        r.blockGain.push_back(alc.gain());
    }
    return r;
}

// The control: the same decision rule, held for the whole block.
Run runHeld(const std::vector<float>& in, double micGain)
{
    Run r;
    r.pre = preOf(in, micGain);
    r.out.resize(in.size());
    double gain = 1.0;
    for (std::size_t off = 0; off + kBlock <= in.size(); off += kBlock) {
        float peak = 0.0f;
        for (std::size_t s = 0; s < kBlock; ++s)
            peak = std::max(peak, std::fabs(r.pre[off + s]));
        if (peak > 1e-6f) {
            const double target = std::min(kTarget / peak, 1.0);
            if (target < gain) {
                gain = target;
            } else {
                const double a = 1.0 - std::exp(
                    -(static_cast<double>(kBlock) / kFs) / kReleaseSec);
                gain += a * (target - gain);
            }
        }
        for (std::size_t s = 0; s < kBlock; ++s)
            r.out[off + s] = std::clamp(
                static_cast<float>(r.pre[off + s] * gain), -1.0f, 1.0f);
        r.blockGain.push_back(gain);
    }
    return r;
}

double peakOf(const std::vector<float>& v)
{
    double mx = 0.0;
    for (float x : v)
        mx = std::max(mx, static_cast<double>(std::fabs(x)));
    return mx;
}

// The applied gain read back from the output, sample by sample, and the
// largest change between neighbours: at a block edge, and anywhere.
struct Steps {
    double edgeDb = 0.0;
    double anyDb = 0.0;
};
Steps gainSteps(const Run& r)
{
    Steps st;
    const std::size_t n = (r.out.size() / kBlock) * kBlock;
    for (std::size_t i = 1; i < n; ++i) {
        // Too close to a zero crossing to divide by; the stimulus keeps the
        // samples either side of every block edge clear of this.
        if (std::fabs(r.pre[i]) < 1e-3f || std::fabs(r.pre[i - 1]) < 1e-3f)
            continue;
        const double g1 = r.out[i] / r.pre[i];
        const double g0 = r.out[i - 1] / r.pre[i - 1];
        const double db = std::fabs(20.0 * std::log10(g1 / g0));
        st.anyDb = std::max(st.anyDb, db);
        if (i % kBlock == 0)
            st.edgeDb = std::max(st.edgeDb, db);
    }
    return st;
}

// One tone, 7.5 degrees off a zero crossing so no sample is near zero, under a
// level that starts below the target, climbs 26 dB in a second and falls back
// in the next.
std::vector<float> sweptTone()
{
    const std::size_t n = 96 * kBlock;   // 2.048 s
    std::vector<float> in(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(n);
        const double tri = t < 0.5 ? 2.0 * t : 2.0 * (1.0 - t);
        const double amp = 0.05 * std::pow(20.0, tri);   // 0.05 .. 1.0 .. 0.05
        in[i] = static_cast<float>(amp * std::sin(
            2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs + kPi / 24.0));
    }
    return in;
}

// #5912's stimulus: 700 + 1900 Hz under a 3.125 Hz raised cosine. One envelope
// period is 7680 samples: 15 blocks and a whole number of cycles of both
// tones, so a rectangular window over it leaks nothing.
constexpr std::size_t kPeriod = 7680;
std::vector<float> twoTone(std::size_t periods)
{
    std::vector<float> in(periods * kPeriod);
    for (std::size_t i = 0; i < in.size(); ++i) {
        const double t = static_cast<double>(i) / kFs;
        const double env = 0.5 * (1.0 - std::cos(2.0 * kPi * 3.125 * t));
        in[i] = static_cast<float>(0.05 * env * (std::sin(2.0 * kPi * 700.0 * t)
                                               + std::sin(2.0 * kPi * 1900.0 * t)));
    }
    return in;
}

// Power of one bin of the last whole period, bin spacing 3.125 Hz.
double binPower(const std::vector<float>& v, std::size_t bin)
{
    const float* x = v.data() + (v.size() - kPeriod);
    std::complex<double> acc{0.0, 0.0};
    const double w = -2.0 * kPi * static_cast<double>(bin)
                   / static_cast<double>(kPeriod);
    for (std::size_t n = 0; n < kPeriod; ++n)
        acc += static_cast<double>(x[n])
             * std::complex<double>(std::cos(w * static_cast<double>(n)),
                                    std::sin(w * static_cast<double>(n)));
    return std::norm(acc / static_cast<double>(kPeriod));
}

// Everything the levelling put more than 300 Hz from both tones, 0..12 kHz,
// relative to the two tones together. The stimulus has nothing there (its own
// sidebands sit 3.125 Hz from each tone), so this is gain motion fast enough
// to be heard as roughness and to widen the signal.
double splatterDbc(const std::vector<float>& v)
{
    constexpr std::size_t kBin700 = 224;    // 700 / 3.125
    constexpr std::size_t kBin1900 = 608;
    constexpr std::size_t kGuard = 96;      // 300 Hz
    const double tones = binPower(v, kBin700) + binPower(v, kBin1900);
    double far = 0.0;
    for (std::size_t b = 1; b < kPeriod / 2; ++b) {
        const bool near700 = b + kGuard >= kBin700 && b <= kBin700 + kGuard;
        const bool near1900 = b + kGuard >= kBin1900 && b <= kBin1900 + kGuard;
        if (!near700 && !near1900)
            far += binPower(v, b);
    }
    return 10.0 * std::log10((far + 1e-300) / (tones + 1e-300));
}

// #5912's own reading: the largest bin at one of the first six multiples of
// the block rate (46.875 Hz, 15 bins) either side of either tone, relative to
// that tone. `beside` is the same reading one bin off each of those, which
// shows whether the block-rate bins stand out from their neighbours.
struct BlockRate {
    double onDbc = -400.0;
    double besideDbc = -400.0;
};
BlockRate blockRateSidebands(const std::vector<float>& v)
{
    BlockRate r;
    for (const std::size_t tone : {std::size_t{224}, std::size_t{608}}) {
        const double ref = binPower(v, tone) + 1e-300;
        for (std::size_t k = 1; k <= 6; ++k) {
            for (const std::size_t bin : {tone - 15 * k, tone + 15 * k}) {
                r.onDbc = std::max(r.onDbc,
                    10.0 * std::log10((binPower(v, bin) + 1e-300) / ref));
                r.besideDbc = std::max(r.besideDbc,
                    10.0 * std::log10((binPower(v, bin + 1) + 1e-300) / ref));
            }
        }
    }
    return r;
}

}  // namespace

int main()
{
    // ── 1. The step at the block edge ──────────────────────────────────────
    {
        const std::vector<float> in = sweptTone();
        const Run held = runHeld(in, 10.0);
        const Run alc = runAlc(in, 10.0);
        const Steps hs = gainSteps(held);
        const Steps as = gainSteps(alc);
        std::fprintf(stderr,
                     "swept tone, 0 to -21 dB of reduction and back:\n"
                     "  held per block  largest gain step at a block edge "
                     "%.4f dB, anywhere %.4f dB\n"
                     "  this stage      largest gain step at a block edge "
                     "%.4f dB, anywhere %.4f dB\n",
                     hs.edgeDb, hs.anyDb, as.edgeDb, as.anyDb);
        check(hs.edgeDb > 0.2,
              "control: the instrument reads a block-edge step on a held gain");
        check(as.edgeDb < 0.02,
              "the gain does not step at a block edge");
        check(as.anyDb < 0.05,
              "and does not step anywhere else in the block instead");
        check(peakOf(alc.out) <= kTarget * (1.0 + 1e-6),
              "the swept tone never leaves the stage above the target");
        check(alc.blockGain == held.blockGain,
              "the gain reached at each block's end is the held decision, "
              "exactly");
    }

    // ── 2. What the step costs: energy thrown away from the tones ──────────
    {
        std::fprintf(stderr, "two tones under a 3.125 Hz envelope, energy more "
                             "than 300 Hz from both tones:\n");
        const std::vector<float> in = twoTone(12);
        for (const double micGain : {1.0, 10.0, 31.6, 100.0}) {
            const Run held = runHeld(in, micGain);
            const Run alc = runAlc(in, micGain);
            const double h = splatterDbc(held.out);
            const double a = splatterDbc(alc.out);
            double minGain = 1.0;
            for (std::size_t b = alc.blockGain.size() - 15;
                 b < alc.blockGain.size(); ++b)
                minGain = std::min(minGain, alc.blockGain[b]);
            std::fprintf(stderr,
                         "  mic gain %6.1f  reduction %6.2f dB  held %8.2f dBc  "
                         "this stage %8.2f dBc\n",
                         micGain, 20.0 * std::log10(minGain), h, a);
            const BlockRate hb = blockRateSidebands(held.out);
            const BlockRate ab = blockRateSidebands(alc.out);
            std::fprintf(stderr,
                         "                   block-rate bins: held %8.2f dBc "
                         "(one bin off %8.2f), this stage %8.2f dBc (one bin "
                         "off %8.2f)\n",
                         hb.onDbc, hb.besideDbc, ab.onDbc, ab.besideDbc);
            if (micGain != 1.0) {
                check(ab.onDbc < hb.onDbc - 6.0,
                      "the block-rate bins #5912 read are lower than with a "
                      "held gain");
            }
            check(alc.blockGain == held.blockGain,
                  "two-tone: block-end gains are the held decision, exactly");
            check(peakOf(alc.out) <= kTarget * (1.0 + 1e-6),
                  "two-tone: never above the target");
            if (micGain == 1.0) {
                check(h < -120.0 && a < -120.0,
                      "control: with the ALC idle neither path adds anything");
            } else {
                check(a < h - 6.0,
                      "the path inside the block throws less energy away from "
                      "the tones than a held gain");
            }
        }
    }

    // ── 3. Level and timing are the decision's, unchanged ──────────────────
    {
        // Steady over-level tone: settles on the target, and stays there.
        std::vector<float> steady(40 * kBlock);
        for (std::size_t i = 0; i < steady.size(); ++i)
            steady[i] = static_cast<float>(std::sin(
                2.0 * kPi * 700.0 * static_cast<double>(i) / kFs));
        const Run st = runAlc(steady, 10.0);
        const std::vector<float> tail(st.out.end() - 8 * kBlock, st.out.end());
        std::fprintf(stderr, "steady tone at +20 dB: settled peak %.4f\n",
                     peakOf(tail));
        check(std::fabs(peakOf(tail) - kTarget) < 1e-4,
              "a steady over-level tone settles on the target");
        check(peakOf(st.out) <= kTarget * (1.0 + 1e-6),
              "and never exceeds it, first block included");

        // Under-level audio is untouched: proportional, bit for bit.
        const Run quiet = runAlc(steady, 0.5);
        check(quiet.out == quiet.pre,
              "audio below the target passes through unchanged");

        // 40 dB onset after silence, at the top of the mic slider.
        std::vector<float> onset(20 * kBlock, 0.0f);
        for (std::size_t i = 3 * kBlock + 100; i < onset.size(); ++i)
            onset[i] = static_cast<float>(std::sin(
                2.0 * kPi * 700.0 * static_cast<double>(i) / kFs));
        const Run on = runAlc(onset, 100.0);
        std::size_t atClamp = 0;
        for (float x : on.out)
            atClamp += std::fabs(x) >= 0.999f ? 1 : 0;
        std::fprintf(stderr, "40 dB onset mid-block: peak %.4f, %zu samples at "
                             "the clamp\n", peakOf(on.out), atClamp);
        check(peakOf(on.out) <= kTarget * (1.0 + 1e-6) && atClamp == 0,
              "a 40 dB onset inside a block is held to the target with no "
              "look-ahead and nothing at the clamp");

        // Release: 20 dB down, then quiet. The gain recovers 1 - 1/e of the way
        // in alcReleaseSec, counted in blocks.
        std::vector<float> rel(120 * kBlock);
        for (std::size_t i = 0; i < rel.size(); ++i) {
            const double amp = i < 10 * kBlock ? 1.0 : 0.01;
            rel[i] = static_cast<float>(amp * std::sin(
                2.0 * kPi * 700.0 * static_cast<double>(i) / kFs));
        }
        const Run rr = runAlc(rel, 10.0);
        const double g0 = rr.blockGain[9];
        std::size_t blocks = 0;
        while (10 + blocks < rr.blockGain.size()
               && rr.blockGain[10 + blocks] < g0 + (1.0 - g0) * (1.0 - std::exp(-1.0)))
            ++blocks;
        const double ms = 1000.0 * static_cast<double>(blocks + 1)
                        * static_cast<double>(kBlock) / kFs;
        std::fprintf(stderr, "release from %.2f dB: 63 %% recovered after %.1f "
                             "ms (alcReleaseSec %.0f ms)\n",
                     20.0 * std::log10(g0), ms, 1000.0 * kReleaseSec);
        check(ms >= 1000.0 * kReleaseSec
              && ms < 1000.0 * kReleaseSec + 1000.0 * kBlock / kFs + 1e-9,
              "the release time constant is alcReleaseSec, to the block");
        check(rr.blockGain == runHeld(rel, 10.0).blockGain,
              "release: block-end gains are the held decision, exactly");
        // Rising gain is monotonic inside the block and ends on the decision.
        bool monotonic = true;
        for (std::size_t i = 10 * kBlock + 1; i < rel.size(); ++i) {
            if (std::fabs(rr.pre[i]) < 1e-3f || std::fabs(rr.pre[i - 1]) < 1e-3f)
                continue;
            if (rr.out[i] / rr.pre[i] < rr.out[i - 1] / rr.pre[i - 1] - 1e-6)
                monotonic = false;
        }
        check(monotonic, "the release never dips on its way up");
    }

    // ── 4. Speech-shaped input: the ceiling and the decision, on noise ─────
    {
        std::mt19937 rng(5912);
        std::normal_distribution<double> noise(0.0, 0.2);
        std::vector<float> in(200 * kBlock);
        for (std::size_t i = 0; i < in.size(); ++i) {
            const double syllable = 0.5 * (1.0 - std::cos(
                2.0 * kPi * 4.0 * static_cast<double>(i) / kFs));
            in[i] = static_cast<float>(syllable * syllable * noise(rng));
        }
        const Run alc = runAlc(in, 31.6);
        const Run held = runHeld(in, 31.6);
        std::fprintf(stderr, "syllabic noise at +30 dB: peak %.4f (held %.4f)\n",
                     peakOf(alc.out), peakOf(held.out));
        check(peakOf(alc.out) <= kTarget * (1.0 + 1e-6),
              "syllabic noise never leaves the stage above the target");
        check(alc.blockGain == held.blockGain,
              "syllabic noise: block-end gains are the held decision, exactly");
    }

    // ── 5. Off is unity, and the clamp is then the only limit ──────────────
    {
        Hl2TxAlc alc;
        Hl2TxAlc::Settings s = settings();
        s.enabled = false;
        std::vector<float> in(kBlock, 0.5f), out(kBlock);
        const Hl2TxAlc::Levels lv = alc.process(in, 4.0, s, out);
        check(alc.gain() == 1.0 && lv.micPeak == 2.0f && lv.postPeak == 1.0f
              && out.front() == 1.0f && out.back() == 1.0f,
              "ALC off: unity gain, the hard clamp limits");
    }

    if (g_failures) {
        std::fprintf(stderr, "%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all ALC path checks passed\n");
    return 0;
}
