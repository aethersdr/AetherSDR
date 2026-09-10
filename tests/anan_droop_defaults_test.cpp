#include "core/backends/anan/AnanDroopDefaults.h"

#include <cmath>
#include <cstdio>

using namespace AetherSDR::anan;

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "anan_droop_defaults_test: %s\n", message);
    return 1;
}

int testEveryValidRateReturnsTheSameTable()
{
    const DroopCorrectionTable* first = nullptr;
    for (const int rateKsps : defaultDroopRatesKsps()) {
        const DroopCorrectionTable* t = defaultDroopTableForRate(rateKsps);
        if (!t)
            return fail("a valid DDC0 rate returned no default table");
        if (t->size() != kDroopCorrectionFftSize)
            return fail("default table is not on the droop storage grid");
        if (!first)
            first = t;
        // Rate-independence is the whole claim in AnanDroopDefaults.h: the
        // anti-alias FIR sees the same normalised frequency at every rate.
        // Pin it as identity, not as approximate equality.
        else if (t != first)
            return fail("rates returned different tables; the curve must be rate-independent");
    }
    return 0;
}

int testInvalidRatesAreRefused()
{
    for (const int rateKsps : {0, -48, 47, 49, 24, 3072, 1535}) {
        if (defaultDroopTableForRate(rateKsps))
            return fail("an invalid DDC0 rate returned a table");
    }
    return 0;
}

int testCurveShape()
{
    const DroopCorrectionTable* t = defaultDroopTableForRate(48);
    if (!t)
        return fail("48 ksps returned no table");

    // Never negative and never above the cap the calibrator itself uses --
    // applyDroopCorrectionDb() adds without clamping, so an out-of-range
    // entry here lands straight on the display.
    for (const float v : *t) {
        if (!(v >= 0.0f) || !(v <= 90.0f))
            return fail("a correction entry is negative, NaN, or above the 90 dB cap");
    }

    // Exactly flat at DC. A shipped default that lifts the centre of the
    // span would tilt every noise floor measurement taken through it.
    if (std::fabs((*t)[kDroopCorrectionFftSize / 2]) > 1.0e-4f)
        return fail("the DC bin is not 0 dB");

    // Symmetric about DC -- the filter response is.
    for (std::size_t i = 1; i < kDroopCorrectionFftSize / 2; ++i) {
        const float lo = (*t)[kDroopCorrectionFftSize / 2 - i];
        const float hi = (*t)[kDroopCorrectionFftSize / 2 + i];
        if (std::fabs(lo - hi) > 0.01f)
            return fail("the curve is not symmetric about DC");
    }

    // Monotone non-increasing from the edge inward through the roll-off.
    // Past ~8% in the curve is flat to 0.005 dB and ripples below that, so
    // only the region the correction actually works in is pinned.
    for (std::size_t i = 0; i < 80; ++i) {
        if ((*t)[i + 1] > (*t)[i] + 1.0e-3f)
            return fail("the roll-off region is not monotone toward mid-band");
    }
    return 0;
}

int testAgreesWithHardwareMeasurement()
{
    // Reference points from an in-app calibration sweep on an ANAN-G2
    // (gateware 27, 50 ohm dummy load), over the bins where the correction
    // does real work. These are MEASURED dB, deliberately kept in-tree so a
    // gateware retune shows up as a test failure rather than as a silent
    // display regression.
    //
    // Only the bins where the correction is big enough to discriminate.
    // Points further in (bins 61/66/71, 0.4-2.1 dB) were dropped: at that
    // depth the sweep's own run-to-run scatter exceeds the correction, so
    // any tolerance wide enough to accept honest noise also accepts an
    // ALL-ZERO table. Bin 61 makes the case -- 48 ksps measured 0.67 dB
    // while the other five rates read 1.67-2.18 against a derived 2.13, so
    // the outlier is the measurement, not the curve.
    //
    // Tolerance scales with each point's magnitude, floored at 0.8 dB to
    // cover that same run-to-run spread.
    struct Point { std::size_t bin; float measuredDb; };
    static constexpr Point kMeasured[] = {
        {41, 15.41f}, {46, 10.41f}, {51, 6.05f}, {56, 4.66f},
    };

    const DroopCorrectionTable* t = defaultDroopTableForRate(48);
    if (!t)
        return fail("48 ksps returned no table");
    for (const Point& p : kMeasured) {
        const float tolerance = std::max(0.8f, 0.25f * p.measuredDb);
        if (std::fabs((*t)[p.bin] - p.measuredDb) > tolerance)
            return fail("derived curve disagrees with the ANAN-G2 measurement "
                        "beyond tolerance -- check the gateware version");
    }

    // Guard the guard: the table this test would accept must not be one the
    // hardware contradicts. An all-zero table has to fail, or the points
    // above are decoration.
    DroopCorrectionTable zeros{};
    zeros.fill(0.0f);
    bool zerosRejected = false;
    for (const Point& p : kMeasured) {
        const float tolerance = std::max(0.8f, 0.25f * p.measuredDb);
        if (std::fabs(zeros[p.bin] - p.measuredDb) > tolerance) {
            zerosRejected = true;
            break;
        }
    }
    if (!zerosRejected)
        return fail("the hardware reference points cannot distinguish the "
                    "derived curve from an empty table");
    return 0;
}

}  // namespace

int main()
{
    int failures = 0;
    failures += testEveryValidRateReturnsTheSameTable();
    failures += testInvalidRatesAreRefused();
    failures += testCurveShape();
    failures += testAgreesWithHardwareMeasurement();
    if (failures == 0)
        std::printf("anan_droop_defaults_test: all checks passed\n");
    return failures;
}
