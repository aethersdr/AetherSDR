// Unit test for computeCenters() — pure-math helper that drives the ATU
// pre-tune sweep.  Reference oracle is the IARU Region 1 per-band point
// count table from issue #2624.  Closes #2648.

#include "gui/AtuPreTuneCenters.h"

#include <QVector>

#include <cmath>
#include <iostream>

using namespace AetherSDR;

namespace {

int g_pass = 0;
int g_fail = 0;

bool expect(bool condition, const char* label)
{
    if (condition) {
        std::cout << "[ OK ] " << label << '\n';
        ++g_pass;
    } else {
        std::cout << "[FAIL] " << label << '\n';
        ++g_fail;
    }
    return condition;
}

bool nearlyEqual(double a, double b, double tol = 1e-4)
{
    return std::abs(a - b) < tol;
}

} // namespace

int main()
{
    // ── IARU R1 reference table (issue #2624) ─────────────────────────
    // Band         low        high      seg_kHz  expected_points
    expect(computeCenters(1.800, 2.000, 9).size() == 23,
           "160m IARU R1 (1.800-2.000, 9 kHz)  → 23 points");
    expect(computeCenters(3.500, 3.800, 9).size() == 34,
           "80m IARU R1  (3.500-3.800, 9 kHz)  → 34 points");
    expect(computeCenters(5.3515, 5.3665, 9).size() == 2,
           "60m IARU R1  (5.3515-5.3665, 9 kHz)→ 2 points");
    expect(computeCenters(7.000, 7.200, 25).size() == 8,
           "40m IARU R1  (7.000-7.200, 25 kHz) → 8 points");
    expect(computeCenters(10.100, 10.150, 25).size() == 2,
           "30m IARU R1  (10.100-10.150, 25 kHz)→ 2 points");
    expect(computeCenters(14.000, 14.350, 51).size() == 7,
           "20m IARU R1  (14.000-14.350, 51 kHz)→ 7 points");
    expect(computeCenters(18.068, 18.168, 51).size() == 2,
           "17m IARU R1  (18.068-18.168, 51 kHz)→ 2 points");
    expect(computeCenters(21.000, 21.450, 75).size() == 6,
           "15m IARU R1  (21.000-21.450, 75 kHz)→ 6 points");
    expect(computeCenters(24.890, 24.990, 75).size() == 2,
           "12m IARU R1  (24.890-24.990, 75 kHz)→ 2 points");
    expect(computeCenters(28.000, 29.700, 75).size() == 23,
           "10m IARU R1  (28.000-29.700, 75 kHz)→ 23 points");
    expect(computeCenters(50.000, 52.000, 101).size() == 20,
           "6m IARU R1   (50.000-52.000, 101 kHz)→ 20 points");

    // ── First-center spacing (verifies half-segment offset) ───────────
    const auto m160 = computeCenters(1.800, 2.000, 9);
    expect(!m160.isEmpty() && nearlyEqual(m160.first(), 1.8045),
           "160m first center = 1.8045 MHz (low + 9/2 kHz)");
    const auto m40 = computeCenters(7.000, 7.200, 25);
    expect(!m40.isEmpty() && nearlyEqual(m40.first(), 7.0125),
           "40m first center  = 7.0125 MHz (low + 25/2 kHz)");

    // ── Last-segment clamp (extra center if room at the top) ──────────
    // 60m IARU R1 spans 15 kHz with 9 kHz segments — 1 even-stride
    // center at 5.35600, then a clamped center at high - 9/2 = 5.36200.
    const auto m60 = computeCenters(5.3515, 5.3665, 9);
    expect(m60.size() == 2 && nearlyEqual(m60.last(), 5.3620),
           "60m clamp last center to high - seg/2 (5.3620 MHz)");

    // ── Edge cases ────────────────────────────────────────────────────
    expect(computeCenters(0.0, 0.0, 9).isEmpty(),
           "zero range returns empty");
    expect(computeCenters(2.0, 1.5, 9).isEmpty(),
           "high < low returns empty");
    expect(computeCenters(1.800, 2.000, 0).isEmpty(),
           "segmentKhz=0 returns empty");
    expect(computeCenters(1.800, 2.000, -9).isEmpty(),
           "segmentKhz<0 returns empty");
    expect(computeCenters(14.000, 14.001, 51).isEmpty(),
           "range smaller than one segment returns empty");
    expect(computeCenters(14.000, 14.051, 51).size() == 1,
           "range exactly one segment wide → 1 center (no clamp duplicate)");

    std::cout << '\n' << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
