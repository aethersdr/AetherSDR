// Waterfall "NB Blank" impulse test (#277) on both row kinds — header-only,
// pure logic, no Qt.
//
// The defect pinned here shipped because the detection was inline in
// SpectrumWidget, which links into no test. The blanker compared a row's mean
// with `baseline * threshold` behind a `baseline > 0` guard. A Flex tile is a
// positive intensity and passes; a host-computed pan frame reused as the row
// (HL2, ANAN, RTL-SDR) is negative dBm and never does, so the control was
// enabled, persisted and inert. On an HL2 a 35 to 37 dB step in the floor went
// through with NB Blank on at 1.05 and at 1.95 exactly as with it off.
//
// Four blocks:
//   1. dBm rows: the decisions the operator would see.
//   2. dBm rows through the widget's own ring: a level that stays does not
//      freeze the waterfall, and a poisoned ring recovers.
//   3. Tile rows: the same decisions as before, against the original
//      expression and against hand-computed cases.
//   4. The call site, read as text, because the widget cannot be linked.

#include "gui/WaterfallImpulseBlanker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace WB = AetherSDR::WaterfallImpulseBlanker;
using WB::RowKind;

static int g_total = 0;
static int g_failed = 0;

static void report(const char* name, bool ok)
{
    ++g_total;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", name);
    } else {
        std::printf("ok:   %s\n", name);
    }
}

// The widget's ring, as updateWaterfallRow keeps it: WF_BLANKER_N = 32 row
// means, the baseline is the mean of the entries filled so far.
struct Ring {
    static constexpr int kN = 32;
    float values[kN]{};
    int idx = 0;
    int count = 0;

    float baseline() const
    {
        float sum = 0.0f;
        for (int i = 0; i < count; ++i)
            sum += values[i];
        return count > 0 ? sum / count : 0.0f;
    }

    // Feeds one row; returns true when the blanker replaced it.
    bool feed(RowKind kind, float rowMean, float threshold)
    {
        const WB::Decision d =
            WB::decide(kind, count, baseline(), rowMean, threshold);
        values[idx] = d.ringValue;
        idx = (idx + 1) % kN;
        if (count < kN)
            ++count;
        return d.impulse;
    }
};

// The detection exactly as #277 left it in SpectrumWidget::updateWaterfallRow.
static WB::Decision originalTileDecision(int ringCount, float baseline,
                                         float rowMean, float threshold)
{
    if (ringCount >= 8 && baseline > 0.0f && rowMean > baseline * threshold)
        return {true, std::min(rowMean, baseline * 1.05f)};
    return {false, rowMean};
}

static bool sameFloat(float a, float b)
{
    return std::memcmp(&a, &b, sizeof a) == 0;
}

static bool near(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

static std::string collapseWhitespace(const std::string& text)
{
    std::string out;
    bool inSpace = false;
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            inSpace = true;
            continue;
        }
        if (inSpace && !out.empty())
            out.push_back(' ');
        inSpace = false;
        out.push_back(c);
    }
    return out;
}

int main()
{
    constexpr float kDefault = 1.15f;   // the control's default, slider 15
    constexpr float kLowest = 1.05f;    // slider 5
    constexpr float kHighest = 1.95f;   // slider 95

    // ── 1. dBm rows ────────────────────────────────────────────────────────
    // The floor d170 measured on an HL2 on a dummy load at LNA +30 dB.
    constexpr float kFloor = -147.0f;

    {
        const WB::Decision d =
            WB::decide(RowKind::Dbm, 32, kFloor, kFloor + 20.0f, kDefault);
        report("dBm: a row 20 dB over the floor is an impulse at the default",
               d.impulse);
        report("dBm: the rejected row enters the ring 5 dB over the baseline",
               near(d.ringValue, kFloor + 5.0f, 1e-3f));
    }
    {
        const WB::Decision d =
            WB::decide(RowKind::Dbm, 32, kFloor, kFloor + 0.5f, kDefault);
        report("dBm: an ordinary row is not an impulse", !d.impulse);
        report("dBm: an ordinary row enters the ring as it is",
               sameFloat(d.ringValue, kFloor + 0.5f));
    }
    report("dBm: a row AT the baseline is not an impulse (the dropped-guard "
           "freeze)",
           !WB::decide(RowKind::Dbm, 32, kFloor, kFloor, kDefault).impulse);
    report("dBm: a row below the baseline is not an impulse",
           !WB::decide(RowKind::Dbm, 32, kFloor, kFloor - 30.0f, kDefault)
                .impulse);

    report("dBm: 7 rows of history is not enough",
           !WB::decide(RowKind::Dbm, 7, kFloor, kFloor + 20.0f, kDefault)
                .impulse);
    report("dBm: 8 rows of history is",
           WB::decide(RowKind::Dbm, 8, kFloor, kFloor + 20.0f, kDefault)
               .impulse);
    report("dBm: no history, no impulse",
           !WB::decide(RowKind::Dbm, 0, 0.0f, kFloor, kDefault).impulse);

    // The operator's control: one dB per step, 5 / 15 / 95 dB.
    report("margin is 5 dB at 1.05", near(WB::dbMargin(kLowest), 5.0f, 1e-3f));
    report("margin is 15 dB at 1.15",
           near(WB::dbMargin(kDefault), 15.0f, 1e-3f));
    report("margin is 95 dB at 1.95",
           near(WB::dbMargin(kHighest), 95.0f, 1e-3f));
    struct MarginCase {
        float threshold;
        float excessDb;
        bool impulse;
    };
    const MarginCase margins[] = {
        {kLowest, 4.0f, false},  {kLowest, 6.0f, true},
        {kDefault, 14.0f, false}, {kDefault, 16.0f, true},
        {kHighest, 94.0f, false}, {kHighest, 96.0f, true},
        // d170's stimulus: a 36 dB step. Caught at the low end and at the
        // default, let through at the top, which keeps that run's negative
        // control.
        {kLowest, 36.0f, true},  {kDefault, 36.0f, true},
        {kHighest, 36.0f, false},
    };
    bool marginsOk = true;
    for (const MarginCase& c : margins) {
        if (WB::decide(RowKind::Dbm, 32, kFloor, kFloor + c.excessDb,
                       c.threshold).impulse != c.impulse) {
            marginsOk = false;
            std::printf("      threshold %.2f excess %.1f dB\n",
                        static_cast<double>(c.threshold),
                        static_cast<double>(c.excessDb));
        }
    }
    report("dBm: the margin follows the operator's threshold", marginsOk);

    // A dB difference does not care where the zero of the scale is: the same
    // excess gives the same answer on a quiet band, a loud one, and a
    // baseline above 0 dBm.
    bool levelIndependent = true;
    for (float baseline : {-148.0f, -111.0f, -60.0f, -20.0f, 0.0f, 10.0f}) {
        if (WB::decide(RowKind::Dbm, 32, baseline, baseline + 14.0f, kDefault)
                .impulse
            || !WB::decide(RowKind::Dbm, 32, baseline, baseline + 16.0f,
                           kDefault).impulse) {
            levelIndependent = false;
            std::printf("      baseline %.0f dBm\n",
                        static_cast<double>(baseline));
        }
    }
    report("dBm: the decision does not depend on the absolute level",
           levelIndependent);

    // ── 2. dBm rows through the ring ───────────────────────────────────────
    {
        // A strong band from the first row on: nothing to stand out against.
        Ring ring;
        int blanked = 0;
        for (int i = 0; i < 400; ++i) {
            // +-1 dB of row-to-row movement around -60 dBm.
            const float jitter = static_cast<float>((i * 7) % 5 - 2) * 0.5f;
            blanked += ring.feed(RowKind::Dbm, -60.0f + jitter, kLowest);
        }
        report("dBm: a steady strong band is never blanked", blanked == 0);
    }
    {
        // One impulse row in a quiet band.
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(RowKind::Dbm, kFloor, kDefault);
        const bool hit = ring.feed(RowKind::Dbm, kFloor + 25.0f, kDefault);
        const float after = ring.baseline();
        int later = 0;
        for (int i = 0; i < 100; ++i)
            later += ring.feed(RowKind::Dbm, kFloor, kDefault);
        report("dBm: a single impulse row is replaced", hit);
        report("dBm: it moves the baseline by 5/32 dB, not by 25/32",
               near(after - kFloor, 5.0f / 32.0f, 1e-2f));
        report("dBm: the rows after it are not blanked", later == 0);
    }
    for (float threshold : {kLowest, kDefault}) {
        // The band comes up 36 dB and STAYS (d170's LNA step). The blanker
        // holds the last good row while the baseline climbs, then lets go.
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(RowKind::Dbm, kFloor, threshold);
        int blanked = 0;
        int lastBlanked = -1;
        const int kRows = 2000;
        for (int i = 0; i < kRows; ++i) {
            if (ring.feed(RowKind::Dbm, kFloor + 36.0f, threshold)) {
                ++blanked;
                lastBlanked = i;
            }
        }
        std::printf("      threshold %.2f: %d rows held, last at row %d\n",
                    static_cast<double>(threshold), blanked, lastBlanked);
        // 50 rows is the 2.00 s d170 waited before its grab, at 25 rows/s.
        report("dBm: a 36 dB step is held for more than d170's 50 rows",
               blanked > 50 && blanked == lastBlanked + 1);
        report("dBm: and the waterfall is released, it does not freeze",
               lastBlanked < 300);
        report("dBm: the baseline has reached the new level",
               near(ring.baseline(), kFloor + 36.0f, 1e-2f));
    }
    {
        // Same step with the control at the top: 36 dB is under 95 dB.
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(RowKind::Dbm, kFloor, kHighest);
        int blanked = 0;
        for (int i = 0; i < 200; ++i)
            blanked += ring.feed(RowKind::Dbm, kFloor + 36.0f, kHighest);
        report("dBm: at 1.95 a 36 dB step passes untouched", blanked == 0);
    }
    {
        // The band goes quiet. A falling level is never an impulse.
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(RowKind::Dbm, -60.0f, kLowest);
        int blanked = 0;
        for (int i = 0; i < 100; ++i)
            blanked += ring.feed(RowKind::Dbm, kFloor, kLowest);
        report("dBm: a falling level is never blanked", blanked == 0);
    }
    {
        // One row of -inf (an empty bin). Without the finite-baseline guard
        // every later row is an impulse and writes -inf back: a permanent
        // freeze. With it the ring refills and the blanker works again.
        const float inf = std::numeric_limits<float>::infinity();
        report("dBm: a -inf baseline fails open",
               !WB::decide(RowKind::Dbm, 32, -inf, kFloor, kDefault).impulse);
        report("dBm: a NaN baseline fails open",
               !WB::decide(RowKind::Dbm, 32,
                           std::numeric_limits<float>::quiet_NaN(), kFloor,
                           kDefault).impulse);
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(RowKind::Dbm, kFloor, kDefault);
        ring.feed(RowKind::Dbm, -inf, kDefault);
        int blanked = 0;
        for (int i = 0; i < 64; ++i)
            blanked += ring.feed(RowKind::Dbm, kFloor, kDefault);
        report("dBm: a -inf row does not freeze the waterfall", blanked == 0);
        report("dBm: and the ring is finite again 32 rows later",
               std::isfinite(ring.baseline()));
        report("dBm: after which an impulse is caught again",
               ring.feed(RowKind::Dbm, kFloor + 25.0f, kDefault));
    }

    // ── 3. Tile rows: unchanged ────────────────────────────────────────────
    {
        // Hand-computed, so the table does not merely agree with a copy of
        // the expression. 108 * 1.15 = 124.2; 108 * 1.05 = 113.4.
        struct TileCase {
            const char* name;
            int history;
            float baseline;
            float rowMean;
            float threshold;
            bool impulse;
            float ringValue;
        };
        const TileCase cases[] = {
            {"tile: 125 over a 108 baseline at 1.15 is an impulse",
             32, 108.0f, 125.0f, kDefault, true, 113.4f},
            {"tile: 124 over a 108 baseline at 1.15 is not",
             32, 108.0f, 124.0f, kDefault, false, 124.0f},
            {"tile: 114 over a 108 baseline at 1.05 is an impulse",
             32, 108.0f, 114.0f, kLowest, true, 113.4f},
            {"tile: 113 over a 108 baseline at 1.05 is not",
             32, 108.0f, 113.0f, kLowest, false, 113.0f},
            {"tile: 7 rows of history is not enough",
             7, 108.0f, 200.0f, kDefault, false, 200.0f},
            {"tile: 8 rows of history is",
             8, 108.0f, 200.0f, kDefault, true, 113.4f},
            {"tile: a zero baseline never fires",
             32, 0.0f, 50.0f, kDefault, false, 50.0f},
            {"tile: a negative baseline never fires, as before",
             32, -147.0f, -100.0f, kDefault, false, -100.0f},
            {"tile: a row at the baseline is not an impulse",
             32, 108.0f, 108.0f, kLowest, false, 108.0f},
        };
        for (const TileCase& c : cases) {
            const WB::Decision d = WB::decide(RowKind::TileIntensity, c.history,
                                              c.baseline, c.rowMean,
                                              c.threshold);
            report(c.name, d.impulse == c.impulse
                               && near(d.ringValue, c.ringValue, 1e-3f));
        }
    }
    {
        // And bit for bit against the original expression, over a grid that
        // crosses every boundary: history 0..33, baselines of both signs,
        // row means on either side of baseline * threshold.
        int compared = 0;
        int different = 0;
        int fired = 0;
        const float baselines[] = {-147.0f, -1.0f, 0.0f, 0.5f, 1.0f, 60.0f,
                                   96.0f, 108.0f, 120.0f, 160.0f, 511.99f};
        const float thresholds[] = {1.05f, 1.10f, 1.15f, 1.50f, 1.95f, 2.0f};
        const float factors[] = {-1.0f, 0.0f, 0.5f, 0.99f, 1.0f, 1.04f, 1.05f,
                                 1.06f, 1.14f, 1.15f, 1.16f, 1.49f, 1.51f,
                                 1.94f, 1.96f, 2.0f, 2.01f, 5.0f};
        for (int history = 0; history <= 33; ++history) {
            for (float baseline : baselines) {
                for (float threshold : thresholds) {
                    for (float factor : factors) {
                        for (float offset : {0.0f, 1.0f, -1.0f}) {
                            const float rowMean = baseline * factor + offset;
                            const WB::Decision want = originalTileDecision(
                                history, baseline, rowMean, threshold);
                            const WB::Decision got = WB::decide(
                                RowKind::TileIntensity, history, baseline,
                                rowMean, threshold);
                            ++compared;
                            fired += want.impulse;
                            if (want.impulse != got.impulse
                                || !sameFloat(want.ringValue, got.ringValue))
                                ++different;
                        }
                    }
                }
            }
        }
        std::printf("      %d tile cases, %d of them impulses\n", compared,
                    fired);
        report("tile: every case decides as the original expression did",
               different == 0);
        report("tile: the grid holds both outcomes",
               fired > 1000 && compared - fired > 1000);
    }
    {
        // The two kinds really are different laws: the same numbers, read as
        // a tile, do what they always did.
        report("the kind is what selects the law",
               WB::decide(RowKind::Dbm, 32, 108.0f, 124.0f, kDefault).impulse
                   && !WB::decide(RowKind::TileIntensity, 32, 108.0f, 124.0f,
                                  kDefault).impulse);
    }

    // ── 4. The call site ───────────────────────────────────────────────────
    {
        std::ifstream in(AETHER_SOURCE_DIR "/src/gui/SpectrumWidget.cpp");
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string source = collapseWhitespace(buffer.str());
        report("call site: SpectrumWidget.cpp is readable", !source.empty());
        report("call site: the row kind comes from the declared capability",
               source.find("WaterfallImpulseBlanker::decide( m_panBinsAbsolute "
                           "? WaterfallImpulseBlanker::RowKind::Dbm "
                           ": WaterfallImpulseBlanker::RowKind::TileIntensity, "
                           "m_wfBlankerRingCount, baseline, rowMean, "
                           "m_wfBlankerThreshold);")
                   != std::string::npos);
        report("call site: the ring takes the helper's value",
               source.find("m_wfBlankerRing[m_wfBlankerRingIdx] = "
                           "blankerDecision.ringValue;")
                   != std::string::npos);
        report("call site: the inline ratio test is gone",
               source.find("rowMean > baseline * m_wfBlankerThreshold")
                   == std::string::npos);
    }

    std::printf("\n%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
