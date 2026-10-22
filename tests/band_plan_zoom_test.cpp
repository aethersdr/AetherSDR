#include "models/BandPlanManager.h"
#include "gui/PanZoomModeGate.h"

#include <QCoreApplication>

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

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

    // Exercise the same width selector used by MainWindow, after the plan
    // lookup. These ARRL segment edges fit a native 50 kHz span exactly.
    plan.setSegmentsForTest({
        {7.125, 7.175, "", "", {}},
        {14.025, 14.070, "", "", {}},
    });
    constexpr std::array widthsHz{5000, 10000, 20000, 50000,
                                 100000, 200000, 500000, 1000000};
    for (double centerMhz : {7.150, 14.050}) {
        const auto region = plan.zoomRegionAt(centerMhz, true);
        check(region.has_value(), "regression frequency resolves a plan segment");
        if (region) {
            check(nativePanZoomWidthHz(centerMhz, region->lowMhz,
                                       region->highMhz, widthsHz) == 50000,
                  "an exact 50 kHz fit does not round up to 100 kHz");
        }
    }
    check(nativePanZoomWidthHz(14.049999, 14.025, 14.070, widthsHz) == 50000,
          "one Hz below the detent boundary still fits 50 kHz");
    check(nativePanZoomWidthHz(14.050001, 14.025, 14.070, widthsHz) == 100000,
          "one Hz beyond the detent boundary requires 100 kHz");
    check(nativePanZoomWidthHz(14.044999, 14.025, 14.070, widthsHz) == 100000,
          "the high allocation edge also determines the detent");
    check(nativePanZoomWidthHz(28.4, 28.0, 29.68, widthsHz) == 1000000,
          "a region wider than every detent saturates at the widest");
    check(!nativePanZoomWidthHz(14.05, 14.025, 14.070, {}),
          "an empty detent list is refused");
    for (double invalid : {std::nan(""), std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::max(), 1.0e13, -1.0, 0.0}) {
        check(!nativePanZoomWidthHz(invalid, 14.025, 14.070, widthsHz),
              "invalid center cannot reach an integer conversion");
        check(!nativePanZoomWidthHz(14.05, invalid, 14.070, widthsHz),
              "invalid low edge cannot reach an integer conversion");
        check(!nativePanZoomWidthHz(14.05, 14.025, invalid, widthsHz),
              "invalid high edge cannot reach an integer conversion");
    }
    check(!nativePanZoomWidthHz(14.05, 14.070, 14.025, widthsHz),
          "reversed region edges are refused");

    // MainWindow measures the width from the allocation's own midpoint, so a
    // window dragged off-centre (a SCROLL-F pan) cannot inflate the detent:
    // the 14.025-14.070 segment needs 50 kHz from its midpoint however far
    // the pan centre has wandered.
    const double midpoint = (14.025 + 14.070) / 2.0;
    check(nativePanZoomWidthHz(midpoint, 14.025, 14.070, widthsHz) == 50000,
          "measured from the allocation midpoint, the segment fits 50 kHz");
    check(nativePanZoomWidthHz(14.110, 14.025, 14.070, widthsHz) == 200000,
          "measured from a pan centre dragged 40 kHz off, the same segment would need 200 kHz");
    check(!nativePanZoomWidthHz(14.05, 14.025, 14.025, widthsHz),
          "zero-width region is refused");
    check(!nativePanZoomWidthHz(14.05, 14.025, 14.0250001, widthsHz),
          "region edges that collapse to the same Hz are refused");

    return failures == 0 ? 0 : 1;
}
