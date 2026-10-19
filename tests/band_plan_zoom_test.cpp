#include "models/BandPlanManager.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool matches(const std::optional<BandPlanManager::Region>& range,
             double low, double high)
{
    return range && std::abs(range->lowMhz - low) < 1.0e-9
        && std::abs(range->highMhz - high) < 1.0e-9;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    BandPlanManager plan;
    // Band boundaries come only from the selected plan. The separate 60 m
    // channels belong to one band, while 12 m and 10 m remain distinct.
    plan.setSegmentsForTest({
        {5.3305, 5.3333, "", "", {}},
        {5.3515, 5.3665, "", "", {}},
        {5.4035, 5.4063, "", "", {}},
        {14.000, 14.100, "", "", {}},
        {14.100, 14.350, "", "", {}},
        {24.890, 24.990, "", "", {}},
        {28.000, 28.500, "", "", {}},
        {28.500, 29.700, "", "", {}},
    });

    check(matches(plan.zoomRegionAt(14.225, true), 14.100, 14.350),
          "segment zoom uses the containing plan segment");
    check(matches(plan.zoomRegionAt(14.100, true), 14.100, 14.350),
          "a shared segment edge selects the segment beginning there");
    check(matches(plan.zoomRegionAt(14.225, false), 14.000, 14.350),
          "band zoom combines adjacent plan segments");
    check(matches(plan.zoomRegionAt(5.360, false), 5.3305, 5.4063),
          "band zoom includes discrete 60 m channels");
    check(matches(plan.zoomRegionAt(28.400, false), 28.000, 29.700),
          "10 m stays separate from 12 m");
    check(!plan.zoomRegionAt(13.000, false), "outside the plan does not guess a band");
    check(!plan.zoomRegionAt(std::nan(""), true), "invalid frequency is refused");

    return failures == 0 ? 0 : 1;
}
