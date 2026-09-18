// The measurement primitives behind radiocert.
//
// This test exists because BOTH bugs this branch has fixed in the diagnostic
// landed here, and both were the same one: the correlator was asked about the
// right frequency at the WRONG SAMPLE RATE.
//
//   - stage-rx-sidebands probed a 48 kHz capture at an assumed 24 kHz, read
//     -80 to -109 dB for every mode, and reported "no signal" while the RMS
//     plainly showed a 25 dB tone.
//   - stage-sideband — the stage the whole tool exists for — still had it after
//     the first fix, where it would have turned the sideband verdict into a coin
//     toss on noise. A saturation guard was masking it.
//
// So the assertions below are not really about tonePower() being a correct
// Goertzel. They pin the property the CALLERS depend on: that an assumed rate
// moves the probe off the tone and buries it. That is what makes "read the rate
// out of the capture" a correctness requirement rather than a style preference,
// and it is the regression test neither earlier fix could have.

#include "core/RadioCertificationMath.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR::certmath;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static std::vector<float> tone(double hz, double fs, double seconds, double amp = 0.5)
{
    const int n = static_cast<int>(fs * seconds);
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / fs));
    return v;
}

int main()
{
    // ---- the correlator finds a tone it is pointed at ----
    {
        const auto v = tone(1000.0, 48000.0, 0.5);
        const double at1k = db(tonePower(v, 1000.0, 48000.0));
        const double at3k = db(tonePower(v, 3000.0, 48000.0));
        std::fprintf(stderr, "on-tone %.1f dB, off-tone %.1f dB\n", at1k, at3k);
        // A real sine at amplitude 0.5 correlates to 0.25 -> about -12 dB.
        check(at1k > -14.0 && at1k < -10.0, "1 kHz tone reads about -12 dB at the right rate");
        check(at1k - at3k > 30.0, "an unrelated bin is far below the tone");
    }

    // ---- THE REGRESSION. Wrong rate = wrong bin = looks like silence. ----
    //
    // This is the exact arithmetic of both shipped bugs: 48 kHz data probed at
    // an assumed 24 kHz. The probe lands on 2 kHz and the 1 kHz tone vanishes.
    {
        const auto v = tone(1000.0, 48000.0, 0.5);
        const double right = db(tonePower(v, 1000.0, 48000.0));
        const double wrong = db(tonePower(v, 1000.0, 24000.0));
        std::fprintf(stderr, "rate 48000 -> %.1f dB, assumed 24000 -> %.1f dB\n",
                     right, wrong);
        check(right - wrong > 25.0,
              "assuming 24 kHz on a 48 kHz capture buries a real tone (docs/HERMES.md 1.9)");
        check(wrong < -35.0,
              "the wrong-rate reading is indistinguishable from no signal");
    }

    // ---- a sideband comparison across mismatched rates is meaningless ----
    //
    // Why stage-sideband now refuses to draw a verdict when its two captures
    // disagree about the rate: the same signal measured two ways differs by far
    // more than the sideband ratio it is trying to detect.
    {
        const auto v = tone(1000.0, 48000.0, 0.5);
        const double a = db(tonePower(v, 1000.0, 48000.0));
        const double b = db(tonePower(v, 1000.0, 44100.0));
        std::fprintf(stderr, "same signal, rates 48000 vs 44100: %.1f vs %.1f dB\n", a, b);
        check(std::fabs(a - b) > 6.0,
              "mismatched rates move one reading by more than a sideband verdict's margin");
    }

    // ---- rms() is rate-independent, which is why it masked the bug ----
    {
        const auto v = tone(1000.0, 48000.0, 0.5);
        const double r = db(rms(v));
        std::fprintf(stderr, "rms %.2f dB (0.5 amplitude sine -> -9 dB)\n", r);
        check(r > -10.5 && r < -8.0, "rms of a 0.5 sine is about -9 dB");
        check(db(rms(tone(1000.0, 24000.0, 0.5))) > -10.5,
              "rms reads the same at either rate — it cannot catch a rate error");
    }

    // ---- a drifted reference falls out of a coherent bin, and the band
    //      search recovers it ----
    //
    // Why stage-rx-sidebands searches a band instead of a bin. WWV's frequency
    // is exact; OUR dial's is not. A 1 ppm oscillator error at 10 MHz moves the
    // carrier ~10 Hz, and a 1.5 s coherent integration is a ~0.67 Hz bin — so
    // the tone reads as the noise floor in every mode at once, which looks
    // exactly like a deaf receiver rather than a missed probe. That is
    // indistinguishable from the §1.9 wrong-rate bug from the outside.
    {
        const double fs = 48000.0;
        const auto drifted = tone(1510.0, fs, 1.5);     // expected 1500, off by 10
        const double exact = db(tonePower(drifted, 1500.0, fs));
        const double near  = db(tonePowerNear(drifted, 1500.0, fs, 25.0));
        std::fprintf(stderr, "drifted 10 Hz: exact-bin %.1f dB, band-search %.1f dB\n",
                     exact, near);
        check(exact < -35.0, "a 10 Hz drift buries the tone in an exact-bin probe");
        check(near > -14.0 && near < -10.0, "the band search recovers the drifted tone");
        check(near - exact > 20.0, "band search beats exact bin on a drifted reference");
    }

    // The band search must not invent a tone where there is none.
    {
        const double fs = 48000.0;
        const auto other = tone(3000.0, fs, 1.5);
        check(db(tonePowerNear(other, 1500.0, fs, 25.0)) < -35.0,
              "band search finds nothing when the tone is genuinely elsewhere");
    }

    // ---- degenerate inputs ----
    {
        check(tonePower({}, 1000.0, 48000.0) == 0.0, "empty buffer is zero power");
        check(tonePower(tone(1000.0, 48000.0, 0.1), 1000.0, 0.0) == 0.0,
              "a zero sample rate returns zero rather than dividing by it");
        check(rms({}) == 0.0, "empty buffer is zero rms");
        check(db(0.0) < -200.0, "db(0) is floored, not -inf");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "radio_certification_math_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
