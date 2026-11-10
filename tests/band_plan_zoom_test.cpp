#include "models/BandPlanManager.h"
#include "gui/PanZoomModeGate.h"

#include <QCoreApplication>
#include <QString>

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
    check(matches(plan.zoomRegionAt(5.340, false), 5.3305, 5.4063),
          "band zoom between 60 m channels still finds the band");
    check(!plan.zoomRegionAt(5.340, true), "segment zoom between channels finds no segment");
    check(matches(plan.zoomRegionAt(14.1000000000001, true), 14.100, 14.350)
              && matches(plan.zoomRegionAt(14.0999999999999, true), 14.100, 14.350),
          "a frequency a ulp off a shared edge picks the segment beginning there");


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

    // Native span changes leave the reported centre in place. These receiver
    // frequencies share one segment but need different detents at its edges.
    for (double centerMhz : {14.026, 14.069, 14.050001}) {
        const auto region = plan.zoomRegionAt(centerMhz, true);
        check(region.has_value(), "off-midpoint receiver resolves its segment");
        if (!region) {
            continue;
        }
        const auto widthHz = nativePanZoomWidthHz(
            centerMhz, region->lowMhz, region->highMhz, widthsHz);
        check(widthHz == 100000, "retaining an off-midpoint centre needs 100 kHz");
        if (widthHz) {
            const auto centerHz = std::llround(centerMhz * 1.0e6);
            check(centerHz - *widthHz / 2 <= std::llround(region->lowMhz * 1.0e6)
                      && centerHz + *widthHz / 2 >= std::llround(region->highMhz * 1.0e6),
                  "the requested span covers both edges at the retained radio centre");
        }
    }
    check(nativePanZoomWidthHz(14.0475, 14.025, 14.070, widthsHz) == 50000,
          "a radio already centred on the allocation needs only 50 kHz");

    // Centre-honoring modes (SCROLL-F, our Fixed) request the allocation
    // itself, so it lands on screen however far the window had been moved.
    // Fitting around a 12.500 MHz window instead saturates at 1 MHz and shows
    // 12.0-13.0 MHz, with the 14.025-14.070 MHz segment off screen.
    {
        const auto region = plan.zoomRegionAt(14.050, true);
        check(region.has_value(), "the far-window case resolves its segment");
        if (region) {
            check(nativePanZoomWidthHz(12.500, region->lowMhz, region->highMhz, widthsHz)
                      == 1000000,
                  "a span-only fit around a far window saturates");
            const auto window = nativePanZoomCenteredWindow(region->lowMhz, region->highMhz);
            check(window && window->widthHz == 45000
                      && std::llround(window->centerMhz * 1.0e6) == 14047500,
                  "a centred zoom requests the 45 kHz allocation at its midpoint");
            if (window) {
                const auto centerHz = std::llround(window->centerMhz * 1.0e6);
                check(centerHz - window->widthHz / 2 <= std::llround(region->lowMhz * 1.0e6)
                          && centerHz + window->widthHz / 2 >= std::llround(region->highMhz * 1.0e6),
                      "the centred window covers both allocation edges");
            }
        }
    }
    check(!nativePanZoomCenteredWindow(14.070, 14.025), "a reversed allocation is refused");
    check(!nativePanZoomCenteredWindow(std::nan(""), 14.070), "an invalid edge is refused");

    // The whole decision MainWindow dispatches: span-only placements send no
    // centre; centre-honoring placements send the allocation, and one wider
    // than the window keeps the tuned frequency on screen.
    {
        const auto fit = nativePanZoomRequest(false, 14.069, 14.069, 1.0, 14.025, 14.070, widthsHz);
        check(fit && !fit->centerMhz && std::llround(fit->widthMhz * 1.0e6) == 100000,
              "span-only zoom keeps the radio centre and picks a detent");
        const auto centred = nativePanZoomRequest(true, 12.500, 14.050, 1.0, 14.025, 14.070, widthsHz);
        check(centred && centred->centerMhz
                  && std::llround(*centred->centerMhz * 1.0e6) == 14047500
                  && std::llround(centred->widthMhz * 1.0e6) == 45000,
              "a centred zoom from a far window requests the allocation");
        const auto wide = nativePanZoomRequest(true, 28.900, 28.074, 1.0, 28.000, 29.700, widthsHz);
        check(wide && wide->centerMhz && std::llround(wide->widthMhz * 1.0e6) == 1000000,
              "an allocation wider than the window gets the widest window");
        if (wide && wide->centerMhz) {
            const auto c = std::llround(*wide->centerMhz * 1.0e6);
            check(c - 500000 >= 28000000 && c + 500000 <= 29700000,
                  "the narrowed window stays inside the allocation");
            check(c - 500000 + 50000 <= 28074000 && 28074000 <= c + 500000 - 50000,
                  "and keeps the tuned frequency on screen");
        }
        check(!nativePanZoomRequest(true, 14.05, 14.05, 0.0, 14.025, 14.070, widthsHz),
              "an unknown window limit is refused");
    }

    check(!nativePanZoomWidthHz(14.05, 14.025, 14.025, widthsHz),
          "zero-width region is refused");
    check(!nativePanZoomWidthHz(14.05, 14.025, 14.0250001, widthsHz),
          "region edges that collapse to the same Hz are refused");

    return failures == 0 ? 0 : 1;
}
