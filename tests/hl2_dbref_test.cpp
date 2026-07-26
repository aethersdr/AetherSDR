// Every LNA gain change shifts the absolute signal reference by exactly the same
// amount. If the panadapter displays dBm and the gain moves, the whole trace
// jumps and the waterfall paints a horizontal band that reads as a real on-air
// event. Hl2DbReference exists so the gain value and its compensating offset
// live in one object and cannot drift apart.
//
// This asserts the property that matters: a signal of CONSTANT strength reports
// a CONSTANT dBm across a gain change.

#include "core/backends/hl2/Hl2DbReference.h"

#include <cmath>
#include <cstdio>

using AetherSDR::hl2::Hl2DbReference;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}
static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) < tol; }

int main()
{
    Hl2DbReference ref;

    // Uncalibrated by default: reports dBFS unchanged, exactly as this backend
    // did before the type existed. No silent recalibration.
    check(!ref.isCalibrated(), "defaults to uncalibrated");
    check(near(ref.toDbm(-73.0), -73.0),
          "uncalibrated pass-through at the default gain is identity");
    check(near(ref.offsetDb(), 0.0), "uncalibrated offset at the default gain is zero");

    // The regression this guards: subtracting the gain ABSOLUTELY rather than
    // relative to the reference moved the whole displayed floor by 20 dB.
    ref.setLnaGainDb(Hl2DbReference::kDefaultLnaGainDb);
    check(near(ref.toDbm(-120.0), -120.0),
          "default gain leaves the displayed floor exactly where it was");

    // A fixed antenna signal. Raising the LNA by 20 dB raises the digitised
    // level by 20 dB -- and must NOT change the reported strength.
    ref.setFullScaleDbm(-60.0);
    ref.setLnaGainDb(0.0);
    const double reported = ref.toDbm(-13.0);          // -13 dBFS at 0 dB gain

    ref.setLnaGainDb(20.0);
    check(near(ref.toDbm(-13.0 + 20.0), reported),
          "a 20 dB gain increase does not move the reported dBm");

    ref.setLnaGainDb(-12.0);                            // the AD9866 floor
    check(near(ref.toDbm(-13.0 - 12.0), reported),
          "a 12 dB gain cut does not move the reported dBm");

    ref.setLnaGainDb(48.0);                             // the AD9866 ceiling
    check(near(ref.toDbm(-13.0 + 48.0), reported),
          "full-range gain swing does not move the reported dBm");

    // The spectrum path applies offsetDb() per frame rather than toDbm() per
    // bin; the two must agree or the trace and the S-meter would disagree.
    ref.setLnaGainDb(20.0);
    check(near(-13.0 + ref.offsetDb(), ref.toDbm(-13.0)),
          "offsetDb() and toDbm() agree (spectrum vs S-meter)");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_dbref_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
