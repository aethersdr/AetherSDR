#include "gui/PanRecenterPolicy.h"

#include <cmath>
#include <cstdio>

// Regression pins for the kiwi-display recenter contract. The failure this
// guards: while a KiwiSDR virtual antenna owns a pan's display, the pan's
// radio-side geometry is frozen (#3825/#4081), so PanadapterModel's bandwidth
// is stale the moment the operator zooms. A tune-driven recenter (slice-drag
// edge pan, reveal/pan-follow, center-on-slice) that writes through
// requestPanCenter() re-broadcasts that frozen bandwidth via infoChanged and
// visibly snaps the widget's zoom back to the kiwi-assignment span.

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "pan_recenter_policy_test: %s\n", message);
    return 1;
}

bool nearlyEqual(double a, double b, double epsilon = 1.0e-12)
{
    return std::abs(a - b) <= epsilon;
}

using AetherSDR::PanRecenterPolicy::recenterBandwidthMhz;
using AetherSDR::PanRecenterPolicy::recenterWrite;
using AetherSDR::PanRecenterPolicy::Write;

int testRecenterWrite()
{
    // Flex-display pans always write through radio+model — including during
    // an edge-pan drag, where the radio must catch up with the widget's view.
    if (recenterWrite(false, false) != Write::RadioAndModel) {
        return fail("flex pan must write through radio and model");
    }
    if (recenterWrite(false, true) != Write::RadioAndModel) {
        return fail("flex pan must write through even mid-gesture");
    }

    // Kiwi-display pans never reach the radio or the model: a reveal or
    // pan-follow recenters the widget alone …
    if (recenterWrite(true, false) != Write::WidgetLocal) {
        return fail("kiwi pan reveal/follow must recenter the widget only");
    }
    // … and an edge-pan slice drag writes nothing at all — the SpectrumWidget
    // already advanced its own view for the drag tick.
    if (recenterWrite(true, true) != Write::None) {
        return fail("kiwi pan edge-pan drag must not write anywhere");
    }
    return 0;
}

int testRecenterBandwidth()
{
    // Flex pan: the model's bandwidth is live and authoritative.
    if (!nearlyEqual(recenterBandwidthMhz(false, 0.05, 0.2), 0.2)) {
        return fail("flex pan pairs the recenter with the model bandwidth");
    }

    // Kiwi pan: the widget's live (zoomed) span — NEVER the frozen model
    // value. Pairing 0.2 here is exactly the reported snap-back.
    if (!nearlyEqual(recenterBandwidthMhz(true, 0.05, 0.2), 0.05)) {
        return fail("kiwi pan pairs the recenter with the widget bandwidth");
    }

    // Uninitialized widget span falls back to the model value, matching
    // snapCenterLockForSlice (#4116).
    if (!nearlyEqual(recenterBandwidthMhz(true, 0.0, 0.2), 0.2)) {
        return fail("kiwi pan with zero widget span falls back to the model");
    }
    if (!nearlyEqual(recenterBandwidthMhz(true, -1.0, 0.2), 0.2)) {
        return fail("kiwi pan with negative widget span falls back to the model");
    }
    return 0;
}

} // namespace

int main()
{
    if (const int result = testRecenterWrite(); result != 0) {
        return result;
    }
    return testRecenterBandwidth();
}
