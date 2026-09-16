// Compiles src/core/NnrControls.h, which is the point.
//
// The header's ranges and default markers are assertions about WDSP 2.10's
// source, and they carry static_asserts that hold them to it -- but a
// static_assert in a header nobody includes is never compiled and never fails.
// Until NnrFilter lands (RFC #5684 step 2) this is the translation unit that
// includes it, so the assertions are real.
//
// What this test CANNOT do yet is compare the header against a running WDSP:
// the standalone NNR is not on the aether_wdsp.h facade, and tests reach WDSP
// through WdspChannel rather than its headers (docs/architecture/
// wdsp-integration.md). The cross-check -- construct an NNR, read each control
// back, assert it equals the marker here -- belongs with the facade in step 2,
// and is the thing that will actually catch a refresh moving a default.

#include "core/NnrControls.h"

#include <cstdio>

using namespace AetherSDR::Nnr;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool near(double a, double b) { return (a - b < 1e-9) && (b - a < 1e-9); }

}  // namespace

int main()
{
    // Every advanced control is marked somewhere its slider can actually reach.
    for (const ControlSpec* spec : kAdvancedControls) {
        check(markerIsInRange(*spec),
              "an advanced control's marker is outside its range");
        const double pos = markerPosition(*spec);
        check(pos >= 0.0 && pos <= 1.0,
              "an advanced control's marker position is off the slider");
    }
    check(kAdvancedControls.size() == 6,
          "the advanced control list changed size");

    // The marker positions the tab will draw ticks at.
    check(near(markerPosition(kAlpha), 0.25), "alpha marker moved");
    check(near(markerPosition(kAlphaKnee), 0.25),
          "alpha-knee marker moved");
    check(near(markerPosition(kMaxGain), 0.5), "max-gain marker moved");
    check(near(markerPosition(kSmoothAttack), 0.0),
          "smooth-attack marker moved");
    check(near(markerPosition(kSmoothRelease), 0.0),
          "smooth-release marker moved");
    check(near(markerPosition(kMaskFloor), 0.625), "mask-floor marker moved");

    // The mask floor is a documented operator control, not an advanced one, so
    // it is deliberately absent from the advanced list.
    for (const ControlSpec* spec : kAdvancedControls) {
        check(spec != &kMaskFloor,
              "the mask floor is in the advanced control list");
    }

    // The 0-100 strength slider RFC #5684 §8 settled on maps onto the floor's
    // travel, with the marker landing on WDSP's own default.
    const double atFifty = kMaskFloor.maximum
        + (kMaskFloor.minimum - kMaskFloor.maximum) * 0.5;
    check(near(atFifty, -30.0), "the midpoint of the mask-floor range moved");
    check(near(kMaskFloor.defaultValue, -25.0), "the mask-floor default moved");

    if (failures == 0) {
        std::printf("nnr_controls_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
