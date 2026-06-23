#include "gui/MeterExtremes.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

double identityUnits(double raw)
{
    return raw;
}

void testRecordRejectsNonFinite()
{
    MeterExtremes extremes;

    extremes.record(std::numeric_limits<double>::quiet_NaN(), 0);
    extremes.record(std::numeric_limits<double>::infinity(), 0);

    report("record rejects non-finite samples",
           !extremes.hasData() && extremes.avgRaw() == 0.0);
}

void testWindowExpiryClearsRawBounds()
{
    MeterExtremes extremes;
    MeterExtremes::Tuning tuning;
    tuning.windowSeconds = 0.001;
    extremes.setTuning(tuning);

    extremes.record(-90.0, 0);
    extremes.tick(0, 8, SmartMtrUnits::kScaleMin, identityUnits);
    const bool seeded = extremes.hasData()
        && std::fabs(extremes.minRaw() + 90.0) < 0.001
        && std::fabs(extremes.maxRaw() + 90.0) < 0.001;

    extremes.tick(100, 8, SmartMtrUnits::kScaleMin, identityUnits);

    report("expired window clears raw bounds",
           seeded && !extremes.hasData()
               && extremes.minRaw() == 0.0
               && extremes.maxRaw() == 0.0
               && extremes.avgRaw() == 0.0);
}

void testExternalPeakDoesNotAnimateJustForStandingOff()
{
    MeterExtremes extremes;

    extremes.setExternalPeak(SmartMtrUnits::kScaleMax);
    const bool firstTickMoving = extremes.tick(
        0, 100, SmartMtrUnits::kScaleMin, identityUnits);
    const bool secondTickMoving = extremes.tick(
        100, 8, SmartMtrUnits::kScaleMin, identityUnits);

    report("external peak settles after reaching target",
           firstTickMoving && !secondTickMoving && extremes.hasData());
}

void testExternalPeakStaysAliveOnZeroElapsedTick()
{
    MeterExtremes extremes;

    extremes.setExternalPeak(SmartMtrUnits::kScaleMax);

    report("external peak zero-dt tick keeps pending slew alive",
           extremes.tick(0, 0, SmartMtrUnits::kScaleMin, identityUnits));
}

void testExternalPeakBelowNeedleSettlesAtNeedle()
{
    MeterExtremes extremes;
    const double needle = (SmartMtrUnits::kScaleMin + SmartMtrUnits::kScaleMax) / 2.0;

    extremes.setExternalPeak(SmartMtrUnits::kScaleMin);
    const bool firstTickMoving = extremes.tick(0, 100, needle, identityUnits);
    const bool secondTickMoving = extremes.tick(100, 8, needle, identityUnits);

    report("external peak below needle settles at needle",
           firstTickMoving && !secondTickMoving
               && std::fabs(extremes.maxPosUnits() - needle) < 0.001);
}

} // namespace

int main()
{
    testRecordRejectsNonFinite();
    testWindowExpiryClearsRawBounds();
    testExternalPeakDoesNotAnimateJustForStandingOff();
    testExternalPeakStaysAliveOnZeroElapsedTick();
    testExternalPeakBelowNeedleSettlesAtNeedle();

    return g_failed == 0 ? 0 : 1;
}
