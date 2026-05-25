// Unit tests for the license-class-filtered overload of
// BandPlanManager::contiguousRegionsForBand (added by PR #3050, closing
// #2649).  The load-bearing invariant is FILTER-BEFORE-MERGE: dropping
// disallowed segments before sort+merge keeps adjacent allowed/disallowed
// pairs from collapsing into a single region whose center would fall on a
// disallowed frequency.  A future refactor (STL-algorithm consolidation,
// parallelisation) could silently break this; these tests pin it. (#3060)
//
// Constitution: Principle IV (region-aware data comes from
// BandPlanManager, not BandDefs.h).

#include "models/BandPlanManager.h"

#include <QString>
#include <QVector>

#include <cmath>
#include <iostream>

using namespace AetherSDR;
using Segment = BandPlanManager::Segment;
using Region  = BandPlanManager::Region;

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

bool nearlyEqual(double a, double b, double tol = 1e-9)
{
    return std::abs(a - b) < tol;
}

// Build a Segment with valid color so it would survive loadPlanFromJson().
// Tests bypass the loader via setSegmentsForTest, but we still construct
// segments that would be valid through the JSON path for realism.
Segment makeSeg(double low, double high, const QString& license,
                const QString& label = QString())
{
    Segment s;
    s.lowMhz  = low;
    s.highMhz = high;
    s.label   = label;
    s.license = license;
    s.color   = QColor("#808080");
    return s;
}

} // namespace

int main()
{
    // ── Scenario 1: empty class → no filter ─────────────────────────────
    //   Three-arg overload with "" must return identical regions to the
    //   two-arg overload.  Pins the "no class supplied means class-blind"
    //   contract that the dialog relies on when no licence is selected.
    {
        BandPlanManager mgr;
        QVector<Segment> segs = {
            makeSeg(3.500, 3.600, "E"),
            makeSeg(3.600, 3.800, "G"),
            makeSeg(7.000, 7.300, ""),
        };
        mgr.setSegmentsForTest(segs);

        const auto twoArg   = mgr.contiguousRegionsForBand(3.0, 8.0);
        const auto emptyArg = mgr.contiguousRegionsForBand(3.0, 8.0, QString());
        expect(twoArg.size() == emptyArg.size()
                   && twoArg.size() == 2
                   && nearlyEqual(twoArg[0].lowMhz,  3.500)
                   && nearlyEqual(twoArg[0].highMhz, 3.800)
                   && nearlyEqual(twoArg[1].lowMhz,  7.000)
                   && nearlyEqual(twoArg[1].highMhz, 7.300),
               "scenario 1: empty allowedClass returns same regions as two-arg overload");
    }

    // ── Scenario 2: empty-license segments always pass ──────────────────
    //   A BCN/general-purpose segment carries license == "".  The filter
    //   must let it through for any allowedClass — the comment in the
    //   implementation calls this out explicitly.
    {
        BandPlanManager mgr;
        mgr.setSegmentsForTest({makeSeg(14.000, 14.100, "", "BCN")});

        const auto rT = mgr.contiguousRegionsForBand(14.0, 14.5, "T");
        const auto rG = mgr.contiguousRegionsForBand(14.0, 14.5, "G");
        const auto rE = mgr.contiguousRegionsForBand(14.0, 14.5, "E");
        expect(rT.size() == 1 && nearlyEqual(rT[0].lowMhz, 14.000)
                   && nearlyEqual(rT[0].highMhz, 14.100),
               "scenario 2: empty-license segment passes for class T");
        expect(rG.size() == 1 && nearlyEqual(rG[0].lowMhz, 14.000),
               "scenario 2: empty-license segment passes for class G");
        expect(rE.size() == 1 && nearlyEqual(rE[0].lowMhz, 14.000),
               "scenario 2: empty-license segment passes for class E");
    }

    // ── Scenario 3: class filter drops non-matching segments ────────────
    //   [A: "E"], [B: "G"], [C: "E,G"] — filter by "T" → nothing;
    //   filter by "G" → B + C (two separate regions because A breaks
    //   adjacency between them in the input ordering).
    {
        BandPlanManager mgr;
        QVector<Segment> segs = {
            makeSeg(10.100, 10.110, "E",   "A"),
            makeSeg(10.110, 10.120, "G",   "B"),
            makeSeg(10.120, 10.130, "E,G", "C"),
        };
        mgr.setSegmentsForTest(segs);

        const auto rT = mgr.contiguousRegionsForBand(10.0, 10.2, "T");
        expect(rT.isEmpty(),
               "scenario 3: class T matches no segment → empty");

        const auto rG = mgr.contiguousRegionsForBand(10.0, 10.2, "G");
        // B [10.110, 10.120] and C [10.120, 10.130] are adjacent and both
        // allowed → they merge into one region [10.110, 10.130].
        expect(rG.size() == 1
                   && nearlyEqual(rG[0].lowMhz,  10.110)
                   && nearlyEqual(rG[0].highMhz, 10.130),
               "scenario 3: class G keeps B+C, merged into one region");

        const auto rE = mgr.contiguousRegionsForBand(10.0, 10.2, "E");
        // A [10.100, 10.110] and C [10.120, 10.130]: A and C are NOT
        // adjacent (B sits between them in frequency but is dropped by
        // the filter), so they stay as two regions.  Wait — A.high==B.low
        // and B.high==C.low, so after dropping B, A.high==10.110 and
        // C.low==10.120 are 10 kHz apart, beyond the 1 Hz adjacency tol →
        // two distinct regions.
        expect(rE.size() == 2
                   && nearlyEqual(rE[0].lowMhz,  10.100)
                   && nearlyEqual(rE[0].highMhz, 10.110)
                   && nearlyEqual(rE[1].lowMhz,  10.120)
                   && nearlyEqual(rE[1].highMhz, 10.130),
               "scenario 3: class E keeps A+C as two separate regions");
    }

    // ── Scenario 4: adjacent allowed/disallowed stay separate ───────────
    //   THE invariant.  Segments [3.500–3.510 "E"] and [3.510–3.570 "E,G"]
    //   are exactly adjacent (high of one == low of next, inside the 1 Hz
    //   tolerance).  Filtering by "G" must NOT yield the merged region
    //   [3.500–3.570] — that would put the merged centerpoint inside the
    //   "E"-only sub-band, which a G operator cannot transmit on.  Must
    //   return exactly the C segment: [3.510–3.570].
    {
        BandPlanManager mgr;
        QVector<Segment> segs = {
            makeSeg(3.500, 3.510, "E"),
            makeSeg(3.510, 3.570, "E,G"),
        };
        mgr.setSegmentsForTest(segs);

        const auto rG = mgr.contiguousRegionsForBand(3.0, 4.0, "G");
        expect(rG.size() == 1
                   && nearlyEqual(rG[0].lowMhz,  3.510)
                   && nearlyEqual(rG[0].highMhz, 3.570),
               "scenario 4: adjacent E-only and E,G segments don't merge for G");

        // Sanity: with class "E" both segments survive and merge into one.
        const auto rE = mgr.contiguousRegionsForBand(3.0, 4.0, "E");
        expect(rE.size() == 1
                   && nearlyEqual(rE[0].lowMhz,  3.500)
                   && nearlyEqual(rE[0].highMhz, 3.570),
               "scenario 4 sanity: class E merges adjacent E and E,G segments");
    }

    // ── Scenario 5: class filter is case-sensitive and exact ────────────
    //   "G " (trailing space) is whitespace-trimmed before comparison —
    //   the implementation now trims both sides (symmetry with the
    //   per-code .trimmed() that the segment side has always done).
    //   "GE" must NOT match the "G,E" field (substring-safety).
    {
        BandPlanManager mgr;
        mgr.setSegmentsForTest({makeSeg(21.000, 21.450, "G")});

        const auto rWithSpace = mgr.contiguousRegionsForBand(20.0, 22.0, "G ");
        expect(rWithSpace.size() == 1
                   && nearlyEqual(rWithSpace[0].lowMhz,  21.000)
                   && nearlyEqual(rWithSpace[0].highMhz, 21.450),
               "scenario 5: trailing-whitespace 'G ' is trimmed and matches 'G'");

        const auto rLower = mgr.contiguousRegionsForBand(20.0, 22.0, "g");
        expect(rLower.isEmpty(),
               "scenario 5: lowercase 'g' does NOT match uppercase 'G' (case-sensitive)");

        BandPlanManager mgr2;
        mgr2.setSegmentsForTest({makeSeg(14.000, 14.350, "G,E")});

        const auto rSubstr = mgr2.contiguousRegionsForBand(13.0, 15.0, "GE");
        expect(rSubstr.isEmpty(),
               "scenario 5: 'GE' does NOT match 'G,E' field (substring-safe)");

        // Sanity: "G" alone matches the "G,E" field after split.
        const auto rG = mgr2.contiguousRegionsForBand(13.0, 15.0, "G");
        expect(rG.size() == 1 && nearlyEqual(rG[0].lowMhz, 14.000),
               "scenario 5 sanity: 'G' matches a comma-separated 'G,E' field");
    }

    std::cout << '\n' << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
