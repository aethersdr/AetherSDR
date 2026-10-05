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

#include <algorithm>
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

    // ---- control-domain probes: the COMP snap (#6171 / #6174) ----
    //
    // The IC-7300MK2 wrote PROC as NOR/DX/DX+ (raw 76/153/229) and decoded the
    // confirmation read as a 0..100 percent, clamped to the published maximum 2.
    // NOR and DX snapped to DX+; DX+ held. A probe set without the low end would
    // have certified the control.
    {
        check(domainProbeValues(2) == std::vector<int>({0, 1, 2}),
              "a 0..2 preset control probes every preset");
        check(domainProbeValues(10).size() == 11, "COMP 0..10 probes all eleven steps");
        const auto wide = domainProbeValues(100);
        check(wide.front() == 0 && wide.back() == 100
                  && std::find(wide.begin(), wide.end(), 1) != wide.end()
                  && std::find(wide.begin(), wide.end(), 99) != wide.end(),
              "a 0..100 control probes both ends and their neighbours");

        // Echoes of the 76/153/229 writes; 81 and 151 measured live on v26.10.1.
        const int echoedRaw[] = {81, 151, 229};
        std::vector<int> departures;
        for (int step : domainProbeValues(2)) {
            const int percent = echoedRaw[step] * 100 / 255;
            const int readback = std::min(percent, 2);
            // TransmitModel is optimistic: the write shows at once, the
            // confirmation read lands after it.
            const Readback r = readbackOf({step, step, readback, readback}, step);
            if (!r.held())
                departures.push_back(step);
        }
        check(departures == std::vector<int>({0, 1}),
              "the pre-fix percent decode moves NOR and DX, as the operator saw");

        // The fix: COMP 0..10, written at step*25.5 and echoed at the bin centre
        // floor((step + 0.5) * 256 / 11), decoded raw * 11 / 256.
        bool held = true;
        for (int step : domainProbeValues(10)) {
            const int echo = static_cast<int>(std::floor((step + 0.5) * 256.0 / 11.0));
            held &= readbackOf({step, echo * 11 / 256}, step).held();
        }
        check(held, "the bin-centre decode holds every COMP step");

        const Readback late = readbackOf({5, 5, 3, 3}, 3);
        check(late.held() && late.agreedAt == 2,
              "a model that waits for the radio agrees at the readback");
        check(!readbackOf({5, 5, 5}, 3).held(), "a write never read back is not held");
    }

    // ---- control-domain probes: squelch on at threshold 0 (#6172 / #6175) ----
    //
    // Icom has no squelch enable: "on at 0" writes 0000, and the pre-fix
    // readback derived on = level > 0. The sample therefore has to carry the
    // enable; a level-only comparison reads 0 back and certifies the defect.
    {
        const auto state = [](bool on, int level) { return on ? level : -1; };
        const int written = state(true, 0);
        // SliceModel confirms Icom squelch from the radio, so the model holds
        // the prior state (Off) until the readback, which then says Off again.
        check(!readbackOf({state(false, 0), state(false, 0)}, written).held(),
              "on-at-0 read back as off is caught when the enable is sampled");
        check(readbackOf({0, 0}, 0).held(),
              "the same readback compared on level alone certifies the defect");
        const Readback poll = readbackOf({state(true, 0), state(true, 0), state(false, 0)}, written);
        check(!poll.held() && poll.departedAt == 2,
              "a periodic poll re-asserting off is caught after the confirmation read held");
    }

    // ---- coupled controls: the preamp/ATT interlock (#6178 / #6183) ----
    //
    // Engaging ATT drops the preamp on the radio, unreported. Before the fix the
    // preamp button learned it at the next 3 s controls poll; after, from a read
    // queued 60 ms behind the write.
    {
        std::vector<TimedSample> before;
        for (int ms = 100; ms <= 4000; ms += 100)
            before.push_back({ms, ms < 2000 ? 1 : 0});
        std::vector<TimedSample> after;
        for (int ms = 100; ms <= 4000; ms += 100)
            after.push_back({ms, ms < 200 ? 1 : 0});
        check(settledAtMs(before) == 2000, "pre-fix: the preamp settles at the poll");
        check(settledAtMs(before) > kCoupledSettleBudgetMs, "...which is over budget");
        check(settledAtMs(after) == 200 && settledAtMs(after) <= kCoupledSettleBudgetMs,
              "post-fix: the preamp settles in a round trip");

        // One transition can be lucky: a poll 400 ms after the write passes.
        // Live, before the fix, ATT -> OFF took 0.8 s. That is why the stage
        // runs several transitions, never one.
        std::vector<TimedSample> lucky;
        for (int ms = 100; ms <= 4000; ms += 100)
            lucky.push_back({ms, ms < 400 ? 1 : 0});
        check(settledAtMs(lucky) <= kCoupledSettleBudgetMs,
              "a poll that happens to land early passes one transition");
        check(settledAtMs({}) == -1, "no samples, no settling time");
    }

    // ---- the SQL line against the radio's gate (#6180 / #6184) ----
    //
    // IC-7300MK2, measured live: the 14 03 raw at which 15 01 reads closed, and
    // the steady carrier's pan peak. Level = raw / 2.55. Flex's -160 + level
    // misses by up to ~9 dB at low levels and is close above S9, so the stage
    // must report the carrier strength beside the error.
    {
        struct Point { double raw; double panPeakDb; };
        const Point mk2[] = {{131, -118.0}, {135, -114.7}, {158, -103.4}, {160, -103.2},
                             {199, -78.5}, {206.5, -74.5}, {207, -74.4}, {211, -71.0}};
        int legacyMisses = 0;
        double measuredWorst = 0.0;
        for (const Point& p : mk2) {
            const double level = p.raw / 2.55;
            const double legacy = -160.0 + level - p.panPeakDb;
            const double measured = -194.8 + 1.49 * level - p.panPeakDb;
            legacyMisses += std::fabs(legacy) > kSquelchLineToleranceDb;
            measuredWorst = std::max(measuredWorst, std::fabs(measured));
        }
        std::fprintf(stderr, "SQL line: legacy misses %d of 8, measured worst %.1f dB\n",
                     legacyMisses, measuredWorst);
        check(legacyMisses >= 2, "Flex's scale misses the MK2 gate at low levels");
        check(measuredWorst < kSquelchLineToleranceDb, "the measured MK2 record lands on every gate");
        // The proof carrier: open at level 51, closed at 52, peak -118.2 median.
        check(std::fabs(-160.0 + 52 - -118.2) > kSquelchLineToleranceDb
                  && std::fabs(-194.8 + 1.49 * 52 - -118.2) < 2.0,
              "1480 kHz, preamp off: legacy is 10 dB off, the measured record under 2");
    }

    // Measured by stage-squelch-scale itself on the MK2 (2026-10-05). 1480 kHz
    // with P.AMP1 read -64 dBm: gate at level 70, pan peak -96, and BOTH scales
    // land within tolerance, so above S9 the stage must decline. 1120 kHz behind
    // the 20 dB ATT read -108 dBm: gate 45, peak -124, and only the measured
    // record does.
    {
        const auto miss = [](double offset, double step, int gate, double peak) {
            return std::fabs(offset + step * gate - peak) > kSquelchLineToleranceDb;
        };
        check(!miss(-160.0, 1.0, 70, -96.0) && !miss(-194.8, 1.49, 70, -96.0) && -64.0 > kS9Dbm,
              "above S9 a wrong scale passes too, so the stage calls it inconclusive");
        check(miss(-160.0, 1.0, 45, -124.0) && !miss(-194.8, 1.49, 45, -124.0) && -108.0 < kS9Dbm,
              "below S9 the carrier tells the scales apart");
    }

    // ---- gate search ----
    {
        int probes = 0;
        // raw = ceil(2.55 * level); the MK2 closed between raw 131 and 133.
        const auto mk2 = [&probes](int level) {
            ++probes;
            return static_cast<int>(std::ceil(2.55 * level)) >= 132;
        };
        check(lowestClosedLevel(0, 100, mk2) == 52, "binary search finds the first closed level");
        check(probes <= 9, "in about log2(101) probes");
        check(lowestClosedLevel(0, 100, [](int) { return false; }) == -1,
              "a gate still open at 100 is reported, not guessed");
        check(lowestClosedLevel(0, 100, [](int) { return true; }) == 0,
              "a gate closed at 0 is level 0");
        check(gateClosed(-17.0, -200.0) && !gateClosed(-17.0, -25.0),
              "digital silence is closed; a quieter passband is not");
    }

    // ---- pan peak and floor ----
    {
        std::vector<float> bins(1000, -130.0f);
        bins[500] = -90.0f;     // carrier at the centre bin
        bins[100] = -60.0f;     // a stronger signal well away from it
        const double low = 1'380'000.0, high = 1'580'000.0;   // 200 Hz bins
        const double centre = low + 500.5 * 200.0;
        check(panPeakNear(bins, low, high, centre, 500.0) == -90.0,
              "the peak near the carrier, not the strongest signal on the pan");
        check(std::isnan(panPeakNear(bins, low, high, 2'000'000.0, 500.0)),
              "a carrier off the pan has no peak");
        check(median({3.0, 1.0, 2.0}) == 2.0 && median({1.0, 2.0, 3.0, 10.0}) == 2.5,
              "median of odd and even counts");
        check(std::isnan(median({std::nan("")})), "median of nothing finite is NaN");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "radio_certification_math_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
