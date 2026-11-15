// WDSP's psychoacoustic post-processing (emnr.c post2), as ported into
// SpectralNR. Socket-free and deterministic.
//
// What it pins is the behaviour that made the port non-mechanical: the band
// limit is a FREQUENCY, so it covers the same audio as WDSP does instead of
// inheriting a bin fraction calibrated for WDSP's own geometry, and the stage
// is inert until it is switched on.

#include "core/SpectralNR.h"

#include <cmath>
#include <numbers>
#include <cstdio>
#include <vector>

using AetherSDR::SpectralNR;

namespace {

constexpr int kFftSize = 1024;      // AudioEngine's kNr2FftSize
constexpr int kOverlap = 4;         // AudioEngine's kNr2Overlap
constexpr int kSampleRate = 24000;  // the RX DSP rate
constexpr int kBlock = 256;

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// Noisy speech-ish input: a few harmonics under white noise.
std::vector<float> makeInput(int samples)
{
    std::vector<float> v(samples);
    unsigned rng = 22222;
    for (int i = 0; i < samples; ++i) {
        rng = rng * 1103515245u + 12345u;
        const double noise = (static_cast<double>(rng >> 8) / 8388608.0 - 1.0) * 0.05;
        const double t = static_cast<double>(i) / kSampleRate;
        double tone = 0.0;
        for (int h = 1; h <= 4; ++h) {
            tone += 0.05 * std::sin(2.0 * std::numbers::pi * 300.0 * h * t);
        }
        v[i] = static_cast<float>(tone + noise);
    }
    return v;
}

std::vector<float> run(SpectralNR& nr, const std::vector<float>& in)
{
    std::vector<float> out(in.size(), 0.0f);
    for (std::size_t off = 0; off + kBlock <= in.size(); off += kBlock) {
        nr.process(in.data() + off, out.data() + off, kBlock);
    }
    return out;
}

double rms(const std::vector<float>& v, std::size_t from)
{
    double acc = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < v.size(); ++i, ++n) {
        acc += static_cast<double>(v[i]) * v[i];
    }
    return n ? std::sqrt(acc / n) : 0.0;
}

}  // namespace

int main()
{
    const std::vector<float> input = makeInput(kSampleRate);  // one second

    // The band limit is specified in Hz and converted from the live geometry.
    // WDSP's own default is 0.12 of 2048 bins over 24 kHz = 2871 Hz; at this
    // implementation's 513 bins over 12 kHz the same fraction would be 1435 Hz,
    // so the fraction is exactly what must not be copied.
    {
        SpectralNR nr(kFftSize, kSampleRate, kOverlap);
        const double binHz = static_cast<double>(kSampleRate) / kFftSize;
        nr.setPost2TaperHz(2871.0f);
        const int expected = static_cast<int>(2871.0 / binHz);
        check(nr.post2BinLimit() == expected,
              "the band limit does not follow the configured frequency");
        check(std::abs(nr.post2BinLimit() * binHz - 2871.0) < binHz,
              "the band limit is not within one bin of the requested frequency");

        // The trap, stated as an assertion: had the port copied WDSP's 0.12
        // fraction, the stage would cover this instead.
        const int wdspFractionWouldGive = static_cast<int>(0.12 * (kFftSize / 2 + 1));
        check(wdspFractionWouldGive * binHz < 1500.0,
              "the fraction-based limit is no longer the wrong answer — recheck "
              "the geometry note in SpectralNR::applyPsychoacousticPostProcessing");
        check(nr.post2BinLimit() > wdspFractionWouldGive,
              "the Hz-based limit should be wider than the copied fraction");
    }

    // Off by default, and bit-identical to a run with the stage never touched.
    std::vector<float> baseline;
    {
        SpectralNR nr(kFftSize, kSampleRate, kOverlap);
        check(!nr.post2Run(), "post-processing is not off by default");
        baseline = run(nr, input);
    }
    {
        SpectralNR nr(kFftSize, kSampleRate, kOverlap);
        nr.setPost2Run(false);
        nr.setPost2Nlevel(1.0f);          // would be audible if it ran
        const std::vector<float> out = run(nr, input);
        bool identical = out.size() == baseline.size();
        for (std::size_t i = 0; identical && i < out.size(); ++i) {
            identical = out[i] == baseline[i];
        }
        check(identical, "a disabled stage changed the output");
    }

    // Enabled with nothing to inject: the taper and band limit still apply, so
    // the output changes but stays finite and bounded.
    {
        SpectralNR nr(kFftSize, kSampleRate, kOverlap);
        nr.setPost2Run(true);
        nr.setPost2Nlevel(0.0f);
        const std::vector<float> out = run(nr, input);
        bool finite = true;
        for (float v : out) {
            finite = finite && std::isfinite(v);
        }
        check(finite, "the taper produced non-finite output");
        check(rms(out, kSampleRate / 2) > 0.0, "the taper silenced everything");
    }

    // Injection puts energy back, and -- the part that matters -- a BOUNDED
    // amount of it. The first version of this port scaled the synthetic white
    // term by WDSP's bare constants, which made it 16384x too loud, and a
    // directional "louder than nothing" assertion passed that happily. So the
    // bound is the assertion: at the shipped defaults the stage adds fill, not
    // a new signal.
    {
        SpectralNR quiet(kFftSize, kSampleRate, kOverlap);
        quiet.setPost2Run(true);
        quiet.setPost2Nlevel(0.0f);
        const double quietRms = rms(run(quiet, input), kSampleRate / 2);

        SpectralNR def(kFftSize, kSampleRate, kOverlap);
        def.setPost2Run(true);                 // defaults: nlevel 0.15, factor 0.15
        const std::vector<float> defOut = run(def, input);
        bool finite = true;
        for (float v : defOut) {
            finite = finite && std::isfinite(v);
        }
        check(finite, "injection produced non-finite output");
        const double defRms = rms(defOut, kSampleRate / 2);
        check(defRms > quietRms, "injection did not add energy");
        check(defRms < quietRms * 2.0,
              "the default fill is more than doubling the output -- check the "
              "white term's scale against WDSP's 4 * gain * POST2_NOISE_MAG");

        // And the level control still moves it in the right direction.
        SpectralNR loud(kFftSize, kSampleRate, kOverlap);
        loud.setPost2Run(true);
        loud.setPost2Nlevel(0.5f);
        check(rms(run(loud, input), kSampleRate / 2) > defRms,
              "raising the fill level did not add energy");
    }

    // Every control clamps to its documented range rather than trusting callers.
    {
        SpectralNR nr(kFftSize, kSampleRate, kOverlap);
        nr.setPost2Factor(5.0f);
        check(nr.post2Factor() <= 1.0f, "factor did not clamp");
        nr.setPost2Nlevel(-1.0f);
        check(nr.post2Nlevel() >= 0.0f, "nlevel did not clamp");
        nr.setPost2TaperHz(100000.0f);
        check(nr.post2TaperHz() <= 6000.0f, "taper did not clamp");
        check(nr.post2BinLimit() <= kFftSize / 2 + 1, "band limit exceeds the bins");
        nr.setPost2DecaySeconds(0.0f);
        check(nr.post2DecaySeconds() > 0.0f, "decay did not clamp");
    }

    // The band limit is an operator control now, because enabling the stage
    // lowpasses the audio at it -- an AM or FM listener would otherwise lose
    // their highs with nothing on screen to explain it.
    {
        SpectralNR nr(kFftSize, kSampleRate, kOverlap);
        nr.setPost2TaperHz(5000.0f);
        const int wide = nr.post2BinLimit();
        nr.setPost2TaperHz(1500.0f);
        const int narrow = nr.post2BinLimit();
        check(wide > narrow, "the band limit does not follow the control");
    }

    // The startup dry/wet ramp applies to this stage too: at 100% dry it must
    // not be zeroing the band or injecting anything.
    {
        SpectralNR dry(kFftSize, kSampleRate, kOverlap);
        dry.setPost2Run(true);
        dry.setPost2Nlevel(1.0f);
        std::vector<float> out(kBlock, 0.0f);
        // One block only: the wet ramp has not advanced past zero yet.
        dry.process(input.data(), out.data(), kBlock);
        bool finite = true;
        for (float v : out) {
            finite = finite && std::isfinite(v);
        }
        check(finite, "the first block produced non-finite output");
    }

    if (failures == 0) {
        std::printf("nr2_post2_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
