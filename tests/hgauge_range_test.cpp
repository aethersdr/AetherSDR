// HGauge range-change contract.
//
// The defect this pins (PR #4531) was that HGauge::setRange() re-mapped the
// axis but left MeterSmoother holding the *fraction* it had computed under
// the old range, while HGauge::setValue() early-returns on an unchanged
// reading — so a gauge rescaled during a steady signal painted the old
// fraction against the new scale, with nothing to ever correct it. On the SPE
// applet a real 1000 W read as ~1456 W after a MID->HIGH power-level change.
//
// It is measured from the RENDERED widget rather than from value()/min()/max()
// on purpose: every value-level property read correct throughout the defect
// (that is exactly why it survived review and the automation surface), so an
// assertion that doesn't look at pixels would pass with the bug present.
//
// Mutation check: revert the m_smooth re-target in HGauge::setRange() and
// "rescale re-maps a steady reading" / "an unchanged reading after a rescale
// still paints correctly" both fail.

#include "gui/HGauge.h"

#include <QApplication>
#include <QImage>

#include <cmath>
#include <cstdio>

using AetherSDR::HGauge;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

// Fraction of the bar track that is painted, read straight out of a render of
// the real widget. Bar geometry mirrors HGauge::paintEvent: barX = 0,
// barW = width(), barY = 12, barH = height() - 14.
double paintedFraction(HGauge& gauge)
{
    QImage img(gauge.size(), QImage::Format_ARGB32);
    img.fill(Qt::black);
    gauge.render(&img);

    const int y = 12 + (gauge.height() - 14) / 2;
    int lastFilled = -1;
    for (int x = 1; x < gauge.width() - 1; ++x) {
        const QRgb px = img.pixel(x, y);
        // Track background is 0x0a0a18; every fill zone (green/yellow/red) is
        // well clear of it on at least one channel.
        if (qRed(px) > 0x20 || qGreen(px) > 0x20) {
            lastFilled = x;
        }
    }
    return (lastFilled + 1) / double(gauge.width());
}

bool near(double a, double b, double tol = 0.02)
{
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    // An SPE 1.5K-FA power gauge on the MID axis: 0..1100, red from 1000,
    // yellow from 950 — SpeApplet::setPowerRange(1000, 950, 1100).
    HGauge gauge(0.0f, 1100.0f, 1000.0f, "", "", {}, nullptr, 950.0f);
    gauge.resize(400, 24);

    gauge.setValueImmediate(1000.0f);
    report("a settled reading paints its own fraction of the axis",
           near(paintedFraction(gauge), 1000.0 / 1100.0));

    // The operator presses PWR: MID -> HIGH, i.e. 0..1600 red from 1500.
    // The wattage has not moved.
    gauge.setRange(0.0f, 1600.0f, 1500.0f, {}, 1450.0f);
    const double afterRescale = paintedFraction(gauge);
    report("rescale re-maps a steady reading onto the new axis",
           near(afterRescale, 1000.0 / 1600.0));
    report("the operator does not read a value the gauge was never given",
           near(afterRescale * 1600.0, 1000.0, 40.0));

    // The next status frame carries the same 1000 W. setValue() early-returns
    // on it, so this is the case the old code could never recover from.
    gauge.setValue(1000.0f);
    report("an unchanged reading after a rescale still paints correctly",
           near(paintedFraction(gauge), 1000.0 / 1600.0));

    // ...and the reverse direction (HIGH -> MID), which under-reads rather
    // than over-reads — the worse one on an amplifier.
    gauge.setRange(0.0f, 1100.0f, 1000.0f, {}, 950.0f);
    gauge.setValue(1000.0f);
    report("the shrinking direction re-maps too",
           near(paintedFraction(gauge), 1000.0 / 1100.0));

    // A rescale must not disturb a reading that is legitimately pinned to an
    // end of the scale.
    gauge.setValueImmediate(0.0f);
    gauge.setRange(0.0f, 1600.0f, 1500.0f, {}, 1450.0f);
    report("zero stays empty across a rescale", paintedFraction(gauge) < 0.02);

    // Out-of-range readings stay clamped rather than painting past the track.
    gauge.setValueImmediate(9999.0f);
    gauge.setRange(0.0f, 1100.0f, 1000.0f, {}, 950.0f);
    report("an over-range reading stays clamped to full scale",
           paintedFraction(gauge) > 0.98);

    // Ordinary value changes still animate toward their target rather than
    // snapping — the rescale fix must not turn HGauge into a jumpy gauge.
    HGauge ballistic(0.0f, 100.0f, 90.0f, "", "", {}, nullptr, 80.0f);
    ballistic.resize(400, 24);
    ballistic.setValueImmediate(0.0f);
    ballistic.setValue(100.0f);
    report("a value change still eases toward its target (ballistics intact)",
           paintedFraction(ballistic) < 0.9);

    std::printf("\n%d HGauge range test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
