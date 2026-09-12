#include "gui/FilterPassbandMath.h"
#include "core/backends/RadioCapabilities.h"

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

static void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

int main()
{
    RxFilterControl radioPresets;
    radioPresets.presets = {
        {1, QStringLiteral("FIL1"), 3000},
        {2, QStringLiteral("FIL2"), 2400},
        {3, QStringLiteral("FIL3"), 1800},
    };
    check(hasCompleteRxFilterPresets(radioPresets, 3),
          "a complete radio preset list is safe to present");
    check(!hasCompleteRxFilterPresets(radioPresets, 8),
          "stale FIL metadata is rejected during disconnect list rebuild");
    check(!hasCompleteRxFilterPresets(RxFilterControl{}, 3),
          "an empty radio preset list uses the legacy presentation");

    const double usbScale = passbandDragScaleHzPerPixel(3600, 168);
    const double amScale = passbandDragScaleHzPerPixel(10000, 168);
    check(std::abs(usbScale - (3600.0 / 168.0)) < 0.001,
          "USB/CW drag scale uses the radio's 3.6 kHz ceiling");
    check(std::abs(amScale - (10000.0 / 168.0)) < 0.001,
          "AM drag scale uses the radio's 10 kHz ceiling");
    check(amScale > usbScale,
          "different mode capabilities produce different skirt mappings");

    check(constrainPassbandWidth(300, 310, 50, 3600, PassbandDragEdge::Low)
              == PassbandEdgePair{260, 310},
          "low-edge narrowing anchors the untouched high edge");
    check(constrainPassbandWidth(300, 310, 50, 3600, PassbandDragEdge::High)
              == PassbandEdgePair{300, 350},
          "high-edge narrowing anchors the untouched low edge");
    check(constrainPassbandWidth(-7000, 7000, 200, 10000, PassbandDragEdge::Both)
              == PassbandEdgePair{-5000, 5000},
          "AM symmetric resize clamps to the radio's 10 kHz maximum");
    check(constrainPassbandWidth(-1800, 1800, 50, 3600, PassbandDragEdge::Both)
              == PassbandEdgePair{-1800, 1800},
          "an in-range CW/SSB width remains unchanged");

    if (g_failures == 0) {
        std::printf("filter_passband_math_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
