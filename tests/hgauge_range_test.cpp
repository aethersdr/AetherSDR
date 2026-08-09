// HGauge: the painted fill must follow a range change.
//
// setRange() moves the axis under a value that has not changed. Because
// setValue() early-returns on an unchanged reading, a gauge whose scale is
// re-derived from live telemetry (ACOM auto-range, SPE LOW/MID/HIGH) would
// otherwise keep the fraction computed under the OLD bounds and paint it
// against the NEW ones — for as long as the reading stays steady.
//
// Asserts the derived state (filledFraction) rather than the inputs, because
// value/min/max all read correct while the fill is wrong; that divergence IS
// the defect.

#include "HGauge.h"

#include <QApplication>
#include <QtGlobal>

#include <cmath>
#include <cstdio>

using AetherSDR::HGauge;

namespace {

int failures = 0;

void checkNear(float got, float want, const char* what)
{
    const bool ok = std::fabs(got - want) < 0.001f;
    std::printf("%s  %s (got %.4f, want %.4f)\n", ok ? "[ OK ]" : "[FAIL]",
                what, got, want);
    if (!ok) ++failures;
}

QVector<HGauge::Tick> noTicks() { return {}; }

}  // namespace

int main(int argc, char** argv)
{
    // publishAutomationState() caches the gate in a function-local static on
    // first use, and the first use is the first HGauge constructor — so this
    // has to happen before any gauge exists. Set here rather than as a CMake
    // test property so running the binary directly still exercises the
    // bridge-visibility half.
    qputenv("AETHER_AUTOMATION", "1");

    QApplication app(argc, argv);

    // ── the reported case: steady carrier, axis grows ────────────────────
    // ACOM 600S -> 1200S reflected: 150 W axis becomes 257 W. 120 W of
    // reflected power read 206 W before the fix (80% of the new axis).
    {
        HGauge g(0.0f, 150.0f, 114.0f, "", "", noTicks());
        g.setValueImmediate(120.0f);
        checkNear(g.filledFraction(), 0.8f, "settled on the original axis");

        g.setRange(0.0f, 257.0f, 190.0f, noTicks());
        checkNear(g.filledFraction(), 120.0f / 257.0f,
                  "the fill follows the axis when it grows");

        // The next telemetry frame carries the SAME value — the early-return
        // path that made this permanent.
        g.setValue(120.0f);
        checkNear(g.filledFraction(), 120.0f / 257.0f,
                  "an unchanged reading does not resurrect the old fraction");
    }

    // ── the opposite direction: axis shrinks ─────────────────────────────
    {
        HGauge g(0.0f, 1600.0f, 1500.0f, "", "", noTicks());
        g.setValueImmediate(1000.0f);
        checkNear(g.filledFraction(), 0.625f, "settled on the wide axis");

        g.setRange(0.0f, 1100.0f, 1000.0f, noTicks());
        checkNear(g.filledFraction(), 1000.0f / 1100.0f,
                  "the fill follows the axis when it shrinks");
    }

    // ── a value outside the new axis still clamps ────────────────────────
    {
        HGauge g(0.0f, 1600.0f, 1500.0f, "", "", noTicks());
        g.setValueImmediate(1000.0f);
        g.setRange(0.0f, 600.0f, 550.0f, noTicks());
        checkNear(g.filledFraction(), 1.0f,
                  "a value above the new maximum clamps to full");
    }

    // ── a non-zero minimum is respected ──────────────────────────────────
    // MeterApplet's °C/°F toggle moves the minimum as well as the maximum.
    {
        HGauge g(0.0f, 120.0f, 70.0f, "", "", noTicks());
        g.setValueImmediate(60.0f);              // 50% of 0..120 °C
        checkNear(g.filledFraction(), 0.5f, "settled on the Celsius axis");

        g.setRange(32.0f, 248.0f, 158.0f, noTicks());   // Fahrenheit
        checkNear(g.filledFraction(), (60.0f - 32.0f) / (248.0f - 32.0f),
                  "a non-zero minimum is included in the re-map");
    }

    // ── the range change must not animate ────────────────────────────────
    // The axis moved; the signal did not. A sweep would render a change that
    // never happened, so the new fraction has to be live immediately rather
    // than after the ballistics settle.
    //
    // Driven through setValue() rather than setValueImmediate(), so the re-map
    // lands on a gauge with a sweep genuinely IN FLIGHT. With setValueImmediate
    // the smoother is already settled and this case degenerates into a copy of
    // the first one; the in-flight path is the only behaviour this change
    // alters beyond the steady-reading case.
    {
        HGauge g(0.0f, 150.0f, 114.0f, "", "", noTicks());
        g.setValueImmediate(30.0f);
        g.setValue(120.0f);          // animating 20% -> 80%, not yet arrived
        checkNear(g.filledFraction(), 0.2f,
                  "still mid-sweep before the range change");

        g.setRange(0.0f, 257.0f, 190.0f, noTicks());
        checkNear(g.filledFraction(), 120.0f / 257.0f,
                  "the new fraction is immediate, not animated toward");
    }

    // ── a reversed gauge's fill is the COMPLEMENT of the fraction ────────
    // setReversed() (PhoneCwApplet's compression bar) inverts the mapping at
    // paint time — min means a full bar. filledFraction() stays
    // value-normalised, so anything asserting on the painted width has to take
    // 1 - filledFraction(). Pinned because the accessor's doc comment is the
    // only thing that says so.
    {
        HGauge g(0.0f, 20.0f, 15.0f, "", "", noTicks());
        g.setReversed(true);
        g.setValueImmediate(5.0f);
        checkNear(g.filledFraction(), 0.25f,
                  "a reversed gauge reports the value fraction, not the fill");
        checkNear(1.0f - g.filledFraction(), 0.75f,
                  "the painted width of a reversed gauge is its complement");

        // The re-map is unit-agnostic: it works on a reversed gauge too.
        g.setRange(0.0f, 40.0f, 30.0f, noTicks());
        checkNear(g.filledFraction(), 0.125f,
                  "a reversed gauge re-maps on a range change like any other");
    }

    // ── the bridge sees the DERIVED state, not just the inputs ───────────
    // The whole point of #3845: gaugeValue/gaugeMin/gaugeMax all read correct
    // throughout a stale window, so a bridge assertion over them passes with
    // the defect fully present. gaugeFraction is what makes it visible.
    // (Requires AETHER_AUTOMATION, set as a test property in CMakeLists.txt.)
    {
        HGauge g(0.0f, 150.0f, 114.0f, "", "", noTicks());
        g.setValueImmediate(120.0f);
        checkNear(g.property("gaugeFraction").toFloat(), 0.8f,
                  "gaugeFraction is published for the bridge");

        g.setRange(0.0f, 257.0f, 190.0f, noTicks());
        checkNear(g.property("gaugeValue").toFloat(), 120.0f,
                  "gaugeValue still reads correct across the range change");
        checkNear(g.property("gaugeFraction").toFloat(), 120.0f / 257.0f,
                  "gaugeFraction follows the axis, so the bridge can see it");
    }

    // ── a genuine new reading still works afterwards ─────────────────────
    // The re-map must not leave m_value stale, or setValue()'s early-return
    // would compare the next real reading against the wrong previous value.
    // setValue() ANIMATES (setTarget without snap), so assert via
    // setValueImmediate — the point here is that the new reading is accepted
    // and mapped against the NEW bounds, not how fast the needle travels.
    {
        HGauge g(0.0f, 150.0f, 114.0f, "", "", noTicks());
        g.setValueImmediate(120.0f);
        g.setRange(0.0f, 257.0f, 190.0f, noTicks());

        g.setValueImmediate(200.0f);
        checkNear(g.filledFraction(), 200.0f / 257.0f,
                  "a genuine new reading maps onto the new axis");

        // The early-return still guards what it should: re-asserting the same
        // reading is a no-op rather than a reset.
        g.setValue(200.0f);
        checkNear(g.filledFraction(), 200.0f / 257.0f,
                  "re-asserting the same reading changes nothing");
    }

    std::printf("\n%s\n", failures == 0 ? "hgauge_range_test: all checks passed"
                                        : "hgauge_range_test: FAILURES ABOVE");
    return failures == 0 ? 0 : 1;
}
