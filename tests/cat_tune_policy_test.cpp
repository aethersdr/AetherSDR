// Regression pins for the CAT/rigctld retune policy — the fix for #4497, where
// a band change issued over CAT moved the VFO but left the panadapter on the
// old band.
//
// The recenter is RADIO-side and the only lever is one flag on the wire command:
// "slice tune <id> <mhz> autopan=0" suppresses it, the same command without the
// flag lets the radio recenter and re-band the pan (FlexLib Slice.cs:392). So
// the whole fix reduces to which of SliceModel's two tune calls the CAT seam
// picks — and that choice is invisible to the CAT integration suites, which can
// read the VFO but not the pan. Reverting the recenter arm leaves rigctld
// 165/165, FlexCAT 105/105 and TS-2000 95/95 all green, which is exactly why
// the guard has to live here instead.
//
// Three layers, so a break is localized rather than merely detected:
//   1. PanadapterModel::spanContainsMhz — the predicate that decides the arm.
//   2. RadioModel::tuneSliceForCat      — the seam: rejections, and that an
//                                         out-of-span target really reaches the
//                                         recentering call.
//   3. SliceModel                       — that the two calls emit the commands
//                                         the policy above is named for.

#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace AetherSDR;

static int g_failures = 0;

static void check(bool ok, const QString& what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.toUtf8().constData());
        ++g_failures;
    }
}

// ── 1. The in-span predicate ────────────────────────────────────────────────

static void testSpanContainsMhz()
{
    // A pan the radio has reported: 14.100 MHz centre, 8 kHz span, so the span
    // is [14.096, 14.104].
    PanadapterModel pan(QStringLiteral("0x40000000"));
    pan.setCenterBandwidth(14.100, 0.008);
    check(pan.centerKnown(), QStringLiteral("setCenterBandwidth establishes the centre"));

    check(pan.spanContainsMhz(14.100), QStringLiteral("the centre itself is in span"));
    check(pan.spanContainsMhz(14.1035), QStringLiteral("inside the upper half-span"));
    check(pan.spanContainsMhz(14.0965), QStringLiteral("inside the lower half-span"));
    // Inclusive at exactly ±bandwidth/2 — the edge belongs to the span, so a
    // slice parked on the pan edge is not yanked.
    check(pan.spanContainsMhz(14.104), QStringLiteral("the upper edge is in span"));
    check(pan.spanContainsMhz(14.096), QStringLiteral("the lower edge is in span"));

    check(!pan.spanContainsMhz(14.1041), QStringLiteral("just past the upper edge is out"));
    check(!pan.spanContainsMhz(14.0959), QStringLiteral("just past the lower edge is out"));
    // In-BAND but off-screen: the distinction the policy turns on. A band
    // membership test would leave the operator staring at empty spectrum.
    check(!pan.spanContainsMhz(14.250), QStringLiteral("in-band but off-span is out"));
    check(!pan.spanContainsMhz(7.100),  QStringLiteral("cross-band is out (#4497)"));

    // Until the radio has reported a centre, m_centerMhz is a PLACEHOLDER
    // (14.1) and m_bandwidthMhz a placeholder 0.2 — so without the centerKnown
    // term this fresh pan would answer "in span" for anything within ±100 kHz
    // of a frequency it was never told. Out of span is the safe direction: it
    // recenters, which establishes the centre.
    PanadapterModel fresh(QStringLiteral("0x40000001"));
    check(!fresh.centerKnown(), QStringLiteral("a fresh pan has no known centre"));
    check(!fresh.spanContainsMhz(14.100),
          QStringLiteral("the placeholder centre never counts as in span"));

    // Bandwidth reported without a centre (the setter takes either alone).
    PanadapterModel bwOnly(QStringLiteral("0x40000002"));
    bwOnly.setCenterBandwidth(-1.0, 0.008);
    check(!bwOnly.centerKnown(), QStringLiteral("a negative centre leaves centerKnown alone"));
    check(!bwOnly.spanContainsMhz(14.100),
          QStringLiteral("bandwidth without a centre is never in span"));

    // Span not yet known: a non-positive bandwidth can never contain anything.
    PanadapterModel noSpan(QStringLiteral("0x40000003"));
    noSpan.setCenterBandwidth(14.100, 0.0);
    check(noSpan.centerKnown(), QStringLiteral("centre known even with a zero span"));
    check(!noSpan.spanContainsMhz(14.100),
          QStringLiteral("a zero-width span contains nothing, not even its centre"));
}

// ── 2. The seam ─────────────────────────────────────────────────────────────

// A slice whose pan is unknown to the model resolves to no PanadapterModel, so
// the seam takes the out-of-span arm — which is what lets this test observe the
// recentering call end to end without standing up a pan.
static void testTuneSliceForCat()
{
    RadioModel model;
    SliceModel slice(0);
    QSignalSpy commands(&slice, &SliceModel::commandReady);

    check(!model.tuneSliceForCat(nullptr, 14.100), QStringLiteral("a null slice is rejected"));

    // Boundary validation (Principle VII): reject rather than push the target
    // into the model and broadcast frequencyChanged before the radio refuses it.
    const double implausible[] = {
        0.0,                                        // set_freq 0 / an empty parse
        -5.0,                                       // FA-5000;
        std::numeric_limits<double>::quiet_NaN(),   // a parse that yielded NaN
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        1.0e6,                                      // the ceiling itself is excluded
        2.0e6,                                      // 2 THz
    };
    for (double mhz : implausible) {
        check(!model.tuneSliceForCat(&slice, mhz),
              QStringLiteral("implausible target %1 is rejected").arg(mhz));
        check(!RadioModel::isPlausibleCatTuneMhz(mhz),
              QStringLiteral("isPlausibleCatTuneMhz(%1) is false").arg(mhz));
    }
    check(commands.isEmpty(), QStringLiteral("a rejected target issues no tune"));
    check(qFuzzyIsNull(slice.frequency()),
          QStringLiteral("a rejected target does not move the slice"));

    // The ceiling only bounds the physically impossible — every real allocation,
    // microwave included, stays well inside it.
    check(RadioModel::isPlausibleCatTuneMhz(0.136),      QStringLiteral("2200 m is plausible"));
    check(RadioModel::isPlausibleCatTuneMhz(250000.0),   QStringLiteral("250 GHz is plausible"));

    // A locked slice refuses the tune inside SliceModel, so the seam must report
    // the refusal rather than let the CAT client read success for a tune the
    // radio never made.
    slice.setLocked(true);
    commands.clear();   // setLocked writes "slice lock" itself — not a tune
    check(!model.tuneSliceForCat(&slice, 7.100), QStringLiteral("a locked slice is rejected"));
    check(commands.isEmpty(), QStringLiteral("a locked slice issues no tune"));
    slice.setLocked(false);
    commands.clear();

    // THE FIX. Out of span (no pan resolves) must reach tuneAndRecenter, whose
    // command omits autopan=0 so the RADIO recenters and re-bands the pan. If
    // this arm is ever switched back to setFrequency, #4497 is back and this is
    // the assertion that says so.
    check(model.tuneSliceForCat(&slice, 7.100), QStringLiteral("an out-of-span tune is accepted"));
    check(commands.size() == 1, QStringLiteral("an accepted tune issues exactly one command"));
    const QString cmd = commands.isEmpty() ? QString()
                                           : commands.first().first().toString();
    check(cmd == QStringLiteral("slice tune 0 7.100000"),
          QStringLiteral("out-of-span tunes recenter the pan, got %1").arg(cmd));
    check(!cmd.contains(QStringLiteral("autopan=0")),
          QStringLiteral("the recentering command must NOT suppress autopan"));

    // A retune to the frequency the slice already holds is a legitimate no-op,
    // not a refusal — the seam tests the lock rather than comparing before and
    // after precisely so this case still reports success.
    commands.clear();
    check(model.tuneSliceForCat(&slice, 7.100),
          QStringLiteral("a no-op retune still reports success"));
    check(commands.isEmpty(), QStringLiteral("a no-op retune issues no command"));
}

// ── 3. The two commands the policy chooses between ──────────────────────────

static void testSliceTuneCommands()
{
    SliceModel slice(2);
    QSignalSpy commands(&slice, &SliceModel::commandReady);

    // In-span: autopan=0 keeps the radio from recentering, so external Doppler
    // software (SatPC32 steps every few seconds) does not yank the pan.
    slice.setFrequency(14.101);
    check(commands.size() == 1, QStringLiteral("setFrequency issues one command"));
    check(!commands.isEmpty()
              && commands.first().first().toString()
                     == QStringLiteral("slice tune 2 14.101000 autopan=0"),
          QStringLiteral("setFrequency suppresses the radio-side recenter"));

    commands.clear();
    slice.tuneAndRecenter(14.150);
    check(commands.size() == 1, QStringLiteral("tuneAndRecenter issues one command"));
    check(!commands.isEmpty()
              && commands.first().first().toString()
                     == QStringLiteral("slice tune 2 14.150000"),
          QStringLiteral("tuneAndRecenter lets the radio recenter"));

    // Both bail on a locked slice — the reason the seam has to report false.
    slice.setLocked(true);
    commands.clear();   // setLocked writes "slice lock" itself — not a tune
    slice.setFrequency(14.200);
    slice.tuneAndRecenter(14.300);
    check(commands.isEmpty(), QStringLiteral("a locked slice issues no tune command"));
    check(qFuzzyCompare(slice.frequency(), 14.150),
          QStringLiteral("a locked slice keeps its frequency"));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testSpanContainsMhz();
    testTuneSliceForCat();
    testSliceTuneCommands();

    if (g_failures == 0) {
        std::printf("cat_tune_policy_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "cat_tune_policy_test: %d check(s) failed\n", g_failures);
    return 1;
}
