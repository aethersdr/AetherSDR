#pragma once

#include <array>
#include <type_traits>

// Operator-facing ranges and default markers for WDSP Neural Noise Reduction.
//
// Three of NNR's controls are meant for operators and documented as such in the
// WDSP Guide: on/off, mask floor and model. The rest are tuning parameters
// Warren left out of the guide. Exposing them is this project's decision, so
// picking their ranges -- and marking the value WDSP itself starts from,
// plainly enough that an operator can see it without hunting -- is this
// project's job too.
//
// EVERY NUMBER BELOW IS READ OUT OF WDSP 2.10, not chosen. The tuning defaults
// come from create_dfhead() and create_nnet_slot() (nnet.c), and the min and
// max of each of their ranges is the clamp the corresponding setter already
// enforces, so a control cannot ask for something WDSP will silently refuse.
//
// kMaskFloor is the one exception on both counts, and is annotated as such at
// its definition: its default is set at the create_nnr() call site
// (third_party/wdsp/upstream/RXA.c), not in nnet.c, and setFloor_nnet() clamps
// nothing at all -- its range is the WDSP Guide's recommendation, which makes
// it the only range here that is our policy rather than WDSP's arithmetic.
//
// Re-check all of it on a WDSP refresh: a marker in the wrong place is worse
// than no marker, because it tells the operator a lie about where home is.
//
// This header deliberately has no WDSP dependency. The GUI may not include WDSP
// headers (docs/architecture/wdsp-integration.md), and the NNR tab needs these
// numbers as much as the filter does.

namespace AetherSDR::Nnr {

struct ControlSpec {
    double minimum;
    double maximum;
    // WDSP's own starting value, and therefore where the control draws its
    // "default" marker.
    double defaultValue;
    const char* unit;
};

// --- documented operator controls -------------------------------------------

// How far any one bin may be attenuated. NOT a strength knob: raising it lets
// more of the genuine received noise through, which is the right answer on a
// weak signal because it hands the speech/noise decision back to the listener.
//
// Unlike every other row here, BOTH ends of this range are policy rather than
// enforcement -- setFloor_nnet() clamps nothing -- and the default is the
// literal passed to create_nnr() in RXA.c rather than anything in nnet.c. The
// range is the WDSP Guide's recommendation (-10 dB passes the most noise,
// -50 dB is maximum suppression).
inline constexpr ControlSpec kMaskFloor{-50.0, -10.0, -25.0, "dB"};

// --- undocumented tuning controls -------------------------------------------

// Expansion exponent applied to bin gains that fall below the knee.
// 1.0 disables the branch outright (run_dfhead: `if (d->alpha != 1.0 ...)`), so
// the marker also marks "off". Clamped [0, 4] by setAlpha_dfhead().
inline constexpr ControlSpec kAlpha{0.0, 4.0, 1.0, ""};

// Where that expansion starts, as depth below unity gain.
//
// THE SIGN IS INVERTED relative to how it reads in the source. create_dfhead()
// sets the knee with the literal `-10.0` and the comment "// -10 dB", but
// setKnee_dfhead() computes pow(10, -knee_db/20) and clamps knee_db to [0, 40],
// so the value that reproduces the constructed default is +10, and
// getKnee_dfhead() reads it back as +10. Passing -10 clamps to 0.0 -- a knee at
// unity gain, which applies the expansion to every bin instead of the quiet
// ones. The marker goes at +10.
inline constexpr ControlSpec kAlphaKnee{0.0, 40.0, 10.0, "dB"};

// Time constant of the input conditioner's power tracking.
// Clamped [0.05, 30] by setTau_nnet().
inline constexpr ControlSpec kTau{0.05, 30.0, 2.0, "s"};

// Ceiling on how much any bin may be lifted. Clamped [0, 24] by
// setMaxGain_nnet(); NNET_GMAX_DB is the 12 dB start.
inline constexpr ControlSpec kMaxGain{0.0, 24.0, 12.0, "dB"};

// Per-bin gain smoothing. Both default to 0, and 0/0 bypasses the smoother
// entirely (run_dfhead: `if (d->a_att > 0.0 || d->a_rel > 0.0)`), so these two
// markers sit at the bottom of their travel and mean "off". Clamped [0, 500] by
// setSmooth_dfhead().
inline constexpr ControlSpec kSmoothAttack{0.0, 500.0, 0.0, "ms"};
inline constexpr ControlSpec kSmoothRelease{0.0, 500.0, 0.0, "ms"};

// Every control that is not exposed by default, in the order the tab shows
// them. Each is marked at its defaultValue.
inline constexpr std::array<const ControlSpec*, 6> kAdvancedControls{
    &kAlpha, &kAlphaKnee, &kTau, &kMaxGain, &kSmoothAttack, &kSmoothRelease,
};

// Where the default sits along the control's travel, 0.0 at the minimum and 1.0
// at the maximum -- what a tick-drawing widget needs to place the marker. Both
// smoothing controls default to their minimum and so mark at 0.0.
//
// Correct only for a control whose slider ASCENDS with its value, which is
// every one of the advanced controls. kMaskFloor is the exception and must use
// maskFloorMarkerPosition() below: its slider is a 0..100 STRENGTH that runs
// the opposite way to the dB value, so this formula mirrors the mark about the
// centre -- it put it at 62.5% when the default sits at 37.5%.
constexpr double markerPosition(const ControlSpec& spec)
{
    return (spec.defaultValue - spec.minimum) / (spec.maximum - spec.minimum);
}

// ── The mask floor's strength mapping ────────────────────────────────────────
//
// The operator control is a 0..100 strength where MORE means MORE suppression,
// so it runs from kMaskFloor.maximum (-10 dB, the least) down to .minimum
// (-50 dB, the most). Both directions live here because they have to agree:
// the default strength, the marker the tab draws, and the dB the filter asks
// WDSP for are three views of one mapping, and deriving them separately is
// exactly how the shipped default ended up 5 dB away from WDSP's.

constexpr double maskFloorForStrength(double strength)
{
    const double t = (strength < 0.0 ? 0.0 : (strength > 100.0 ? 100.0 : strength)) / 100.0;
    return kMaskFloor.maximum + (kMaskFloor.minimum - kMaskFloor.maximum) * t;
}

constexpr double strengthForMaskFloor(double floorDb)
{
    return 100.0 * (kMaskFloor.maximum - floorDb)
                 / (kMaskFloor.maximum - kMaskFloor.minimum);
}

// Where the tab draws the mask floor's marker: the strength that reproduces
// WDSP's own default, as a fraction of the slider's travel.
constexpr double maskFloorMarkerPosition()
{
    return strengthForMaskFloor(kMaskFloor.defaultValue) / 100.0;
}

// The strength a fresh install starts at -- WDSP's -25 dB, not the middle of
// the slider. 37.5 is not an integer, and the slider is, so 38 is the nearest
// position: -25.2 dB rather than -25.0.
inline constexpr int kMaskFloorDefaultStrength = 38;

// A marker outside its own control's travel would place a tick off the end of
// the slider, so the ranges check themselves at compile time in every
// translation unit that includes this header. tests/nnr_controls_test.cpp is
// the one that exists purely to compile them.
constexpr bool markerIsInRange(const ControlSpec& spec)
{
    return spec.minimum <= spec.defaultValue
        && spec.defaultValue <= spec.maximum
        && spec.minimum < spec.maximum;
}

static_assert(markerIsInRange(kAlpha));
static_assert(markerIsInRange(kAlphaKnee));
static_assert(markerIsInRange(kTau));
static_assert(markerIsInRange(kMaxGain));
static_assert(markerIsInRange(kSmoothAttack));
static_assert(markerIsInRange(kSmoothRelease));
static_assert(markerIsInRange(kMaskFloor));

// The three views of the mask-floor mapping have to agree. If any of these
// fails, the tab is marking one value, the filter is asking WDSP for another,
// and a fresh install is running at a third.
static_assert(maskFloorForStrength(kMaskFloorDefaultStrength) < -24.0
              && maskFloorForStrength(kMaskFloorDefaultStrength) > -26.0,
              "the default strength no longer lands on WDSP's -25 dB floor");
static_assert(maskFloorForStrength(0.0) == kMaskFloor.maximum);
static_assert(maskFloorForStrength(100.0) == kMaskFloor.minimum);
static_assert(maskFloorMarkerPosition() > 0.37 && maskFloorMarkerPosition() < 0.38,
              "the mask-floor marker no longer sits where the default does");

// The knee's sign is the one value here that reads wrong in the WDSP source:
// create_dfhead() writes it as the literal -10.0 with the comment "// -10 dB",
// but setKnee_dfhead() computes pow(10, -knee_db/20) over a [0, 40] clamp, so
// +10 is the value that reproduces the constructed default and -10 clamps to 0
// -- a knee at unity gain, which widens the expansion to every bin. Pin it, so
// that "correcting" the sign fails the build rather than the ear.
static_assert(kAlphaKnee.defaultValue == 10.0,
              "NNR alpha-knee default is +10 dB; -10 clamps to 0 and moves the "
              "knee to unity gain (setKnee_dfhead, nnet.c)");

// 0/0 is what bypasses the smoother (run_dfhead), so these two markers sit at
// the bottom of their travel and mean "off" rather than "a little".
static_assert(kSmoothAttack.defaultValue == 0.0
              && kSmoothRelease.defaultValue == 0.0);

// 1.0 disables the expansion branch outright (run_dfhead's `alpha != 1.0`).
static_assert(kAlpha.defaultValue == 1.0);

}  // namespace AetherSDR::Nnr
