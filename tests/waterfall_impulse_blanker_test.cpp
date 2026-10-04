// Waterfall "NB Blank" impulse test (#277) on both row kinds: header-only, no
// Qt. Pins the absolute-dB decisions, the same through the widget's ring (a
// level that stays must not freeze the waterfall), and the tile decisions
// against the original expression. The last block is a source-text pin.

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
constexpr RowKind kAbsDb = RowKind::AbsoluteDb;

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

// The widget's ring, as updateWaterfallRow keeps it: WF_BLANKER_N row means
// (a static_assert there holds it to kRingRows), the baseline is the mean of
// the entries filled so far.
struct Ring {
    static constexpr int kN = WB::kRingRows;
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

    // Absolute dB rows. The floor is one HL2's on a dummy load at LNA +30 dB.
    constexpr float kFloor = -147.0f;

    {
        const WB::Decision d =
            WB::decide(kAbsDb, 32, kFloor, kFloor + 20.0f, kDefault);
        report("abs dB: 20 dB over the floor is an impulse at the default",
               d.impulse);
        report("abs dB: a rejected row enters the ring 5 dB over the baseline",
               near(d.ringValue, kFloor + 5.0f, 1e-3f));
    }
    {
        const WB::Decision d =
            WB::decide(kAbsDb, 32, kFloor, kFloor + 0.5f, kDefault);
        report("abs dB: an ordinary row is not an impulse", !d.impulse);
        report("abs dB: an ordinary row enters the ring as it is",
               sameFloat(d.ringValue, kFloor + 0.5f));
    }
    report("abs dB: a row AT the baseline is not an impulse (the dropped-guard "
           "freeze)",
           !WB::decide(kAbsDb, 32, kFloor, kFloor, kDefault).impulse);
    report("abs dB: a row below the baseline is not an impulse",
           !WB::decide(kAbsDb, 32, kFloor, kFloor - 30.0f, kDefault)
                .impulse);

    report("abs dB: 7 rows of history is not enough",
           !WB::decide(kAbsDb, 7, kFloor, kFloor + 20.0f, kDefault)
                .impulse);
    report("abs dB: 8 rows of history is",
           WB::decide(kAbsDb, 8, kFloor, kFloor + 20.0f, kDefault)
               .impulse);
    report("abs dB: no history, no impulse",
           !WB::decide(kAbsDb, 0, 0.0f, kFloor, kDefault).impulse);

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
        // A 36 dB step: caught at the low end and at the default, let
        // through at the top.
        {kLowest, 36.0f, true},  {kDefault, 36.0f, true},
        {kHighest, 36.0f, false},
    };
    bool marginsOk = true;
    for (const MarginCase& c : margins) {
        if (WB::decide(kAbsDb, 32, kFloor, kFloor + c.excessDb,
                       c.threshold).impulse != c.impulse) {
            marginsOk = false;
            std::printf("      threshold %.2f excess %.1f dB\n",
                        static_cast<double>(c.threshold),
                        static_cast<double>(c.excessDb));
        }
    }
    report("abs dB: the margin follows the operator's threshold", marginsOk);

    // A dB difference does not care where the zero of the scale is: the same
    // excess gives the same answer on a quiet band, a loud one, and a
    // baseline above 0 dB.
    bool levelIndependent = true;
    for (float baseline : {-148.0f, -111.0f, -60.0f, -20.0f, 0.0f, 10.0f}) {
        if (WB::decide(kAbsDb, 32, baseline, baseline + 14.0f, kDefault)
                .impulse
            || !WB::decide(kAbsDb, 32, baseline, baseline + 16.0f,
                           kDefault).impulse) {
            levelIndependent = false;
            std::printf("      baseline %.0f dB\n",
                        static_cast<double>(baseline));
        }
    }
    report("abs dB: the decision does not depend on the absolute level",
           levelIndependent);

    // Absolute dB rows through the ring.
    {
        // A strong band from the first row on: nothing to stand out against.
        Ring ring;
        int blanked = 0;
        for (int i = 0; i < 400; ++i) {
            // +-1 dB of row-to-row movement around -60 dB.
            const float jitter = static_cast<float>((i * 7) % 5 - 2) * 0.5f;
            blanked += ring.feed(kAbsDb, -60.0f + jitter, kLowest);
        }
        report("abs dB: a steady strong band is never blanked", blanked == 0);
    }
    {
        // One impulse row in a quiet band.
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(kAbsDb, kFloor, kDefault);
        const bool hit = ring.feed(kAbsDb, kFloor + 25.0f, kDefault);
        const float after = ring.baseline();
        int later = 0;
        for (int i = 0; i < 100; ++i)
            later += ring.feed(kAbsDb, kFloor, kDefault);
        report("abs dB: a single impulse row is replaced", hit);
        report("abs dB: it moves the baseline by 5/32 dB, not by 25/32",
               near(after - kFloor, 5.0f / 32.0f, 1e-2f));
        report("abs dB: the rows after it are not blanked", later == 0);
    }
    for (float threshold : {kLowest, kDefault}) {
        // The band comes up 36 dB and stays (an LNA step). The blanker holds
        // the last good row while the baseline climbs, then lets go.
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(kAbsDb, kFloor, threshold);
        int blanked = 0;
        int lastBlanked = -1;
        const int kRows = 2000;
        for (int i = 0; i < kRows; ++i) {
            if (ring.feed(kAbsDb, kFloor + 36.0f, threshold)) {
                ++blanked;
                lastBlanked = i;
            }
        }
        std::printf("      threshold %.2f: %d rows held, last at row %d\n",
                    static_cast<double>(threshold), blanked, lastBlanked);
        // 50 rows is 2.00 s at 25 rows/s.
        report("abs dB: a 36 dB step is held for more than 50 rows",
               blanked > 50 && blanked == lastBlanked + 1);
        report("abs dB: and the waterfall is released, it does not freeze",
               lastBlanked < 300);
        report("abs dB: the baseline has reached the new level",
               near(ring.baseline(), kFloor + 36.0f, 1e-2f));
    }
    {
        // Same step with the control at the top: 36 dB is under 95 dB.
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(kAbsDb, kFloor, kHighest);
        int blanked = 0;
        for (int i = 0; i < 200; ++i)
            blanked += ring.feed(kAbsDb, kFloor + 36.0f, kHighest);
        report("abs dB: at 1.95 a 36 dB step passes untouched", blanked == 0);
    }
    {
        // The band goes quiet. A falling level is never an impulse.
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(kAbsDb, -60.0f, kLowest);
        int blanked = 0;
        for (int i = 0; i < 100; ++i)
            blanked += ring.feed(kAbsDb, kFloor, kLowest);
        report("abs dB: a falling level is never blanked", blanked == 0);
    }
    {
        // One row of -inf (an empty bin). Without the finite-baseline guard
        // every later row is an impulse and writes -inf back: a permanent
        // freeze. With it the ring refills and the blanker works again.
        const float inf = std::numeric_limits<float>::infinity();
        report("abs dB: a -inf baseline fails open",
               !WB::decide(kAbsDb, 32, -inf, kFloor, kDefault).impulse);
        report("abs dB: a NaN baseline fails open",
               !WB::decide(kAbsDb, 32,
                           std::numeric_limits<float>::quiet_NaN(), kFloor,
                           kDefault).impulse);
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(kAbsDb, kFloor, kDefault);
        ring.feed(kAbsDb, -inf, kDefault);
        int blanked = 0;
        for (int i = 0; i < 64; ++i)
            blanked += ring.feed(kAbsDb, kFloor, kDefault);
        report("abs dB: a -inf row does not freeze the waterfall",
               blanked == 0);
        report("abs dB: and the ring is finite again 32 rows later",
               std::isfinite(ring.baseline()));
        report("abs dB: after which an impulse is caught again",
               ring.feed(kAbsDb, kFloor + 25.0f, kDefault));
    }

    {
        // A ring is not a baseline in the other unit, which is why the widget
        // empties it when the row kind changes. Absolute-dB residue under the
        // tile law: the mean turns small and positive, every tile row is an
        // impulse. Tile residue under the dB law fails open.
        Ring ring;
        for (int i = 0; i < 40; ++i)
            ring.feed(kAbsDb, kFloor, kDefault);
        int held = 0;
        for (int i = 0; i < 2000; ++i)
            held += ring.feed(RowKind::TileIntensity, 108.0f, kDefault);
        std::printf("      residue: %d tile rows held\n", held);
        report("kind change: dB residue holds a tile waterfall over 100 rows",
               held > 100);
        Ring other;
        for (int i = 0; i < 40; ++i)
            other.feed(RowKind::TileIntensity, 108.0f, kDefault);
        int heldDb = 0;
        for (int i = 0; i < 2000; ++i)
            heldDb += other.feed(kAbsDb, kFloor, kDefault);
        report("kind change: tile residue holds no dB row", heldDb == 0);
        Ring emptied;
        int heldEmptied = 0;
        for (int i = 0; i < 2000; ++i)
            heldEmptied +=
                emptied.feed(RowKind::TileIntensity, 108.0f, kDefault);
        report("kind change: an emptied ring holds no tile row",
               heldEmptied == 0);
    }

    // Tile rows: unchanged.
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
               WB::decide(kAbsDb, 32, 108.0f, 124.0f, kDefault).impulse
                   && !WB::decide(RowKind::TileIntensity, 32, 108.0f, 124.0f,
                                  kDefault).impulse);
    }

    // Source-text pin. The claim: SpectrumWidget takes the row kind from its
    // m_panBinsAbsolute, writes the helper's value to the ring, keeps no inline
    // ratio test, asserts its ring size and empties the ring when the row kind
    // changes. SpectrumWidget links into no test target, so this pins how the
    // code is written: whitespace is collapsed, any other reformat of those
    // lines turns it red.
    {
        std::ifstream in(AETHER_SOURCE_DIR "/src/gui/SpectrumWidget.cpp");
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string source = collapseWhitespace(buffer.str());
        report("call site: SpectrumWidget.cpp is readable", !source.empty());
        report("call site: the row kind comes from the declared capability",
               source.find("WaterfallImpulseBlanker::decide( m_panBinsAbsolute "
                           "? WaterfallImpulseBlanker::RowKind::AbsoluteDb "
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
        report("call site: the widget's ring size is asserted to be kRingRows",
               source.find("static_assert(WF_BLANKER_N == "
                           "WaterfallImpulseBlanker::kRingRows,")
                   != std::string::npos);
    }

    {
        std::ifstream in(AETHER_SOURCE_DIR "/src/gui/SpectrumWidget.h");
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string header = collapseWhitespace(buffer.str());
        report("kind change: setPanBinsAbsolute empties the ring on a change",
               header.find("void setPanBinsAbsolute(bool on) { "
                           "if (m_panBinsAbsolute != on) "
                           "resetWfBlankerState();")
                   != std::string::npos);
    }

    std::printf("\n%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
