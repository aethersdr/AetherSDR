// Unit tests for the BandDefs.h band edges via BandSettings::bandForFrequency().
//
// These exist because of #4723: 60m's upper edge sat at 5.405 while the 5405 kHz
// channel's 2.8 kHz slot runs to 5.4063, so the top 1.3 kHz of a legal channel
// reported "not 60m". Nothing in the tree noticed. Every frequency->band
// consumer reads the same table — band-stack grouping (BandStackPanel.cpp:165,
// :285), the per-band auto-save cap (MainWindow.cpp:4663), digital-mode band
// matching (MainWindow_DigitalModes.cpp:346), the SWR-sweep range
// (MainWindow_SwrSweep.cpp:241) — and, critically, the TX-filter PTT preflight
// in RadioModel::txFilterFrequencyLimitMessage(), which compares the emission
// envelope against BandDef::highMhz. An edge that is too LOW silently
// misclassifies legal operation; an edge that is too HIGH silently lets an
// out-of-band passband key. Both failure modes are invisible without an
// assertion, so pin the edges here.
//
// Pure arithmetic over constexpr data — no Qt event loop, no rendering, no
// platform-dependent behaviour.

#include "models/BandSettings.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok)
        ++g_failed;
}

bool bandIs(double freqMhz, const char* want)
{
    return BandSettings::bandForFrequency(freqMhz) == QLatin1String(want);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── 60m, the band #4723 was about ────────────────────────────────────────
    // The FCC's encompassing range is 5330.5 - 5406.4 kHz: the low edge is
    // where the 5332 kHz channel's 2.8 kHz slot starts, the high edge is where
    // the 5405 kHz channel's slot ends.
    report("60m low edge 5.3305 is inside the band",
           bandIs(5.3305, "60m"));
    report("60m just below the low edge is not 60m",
           bandIs(5.3304, "GEN"));

    // The regression itself: 5.4063 is the top of a legal USB emission from the
    // 5403.5 dial. The pre-#4723 edge of 5.405 put this outside 60m.
    report("60m 5.4063 (top of the 5405 kHz channel) resolves to 60m",
           bandIs(5.4063, "60m"));
    report("60m 5.4055 (former dead sliver above 5.405) resolves to 60m",
           bandIs(5.4055, "60m"));
    report("60m high edge 5.4064 is inside the band",
           bandIs(5.4064, "60m"));

    // The other direction, which matters for the PTT preflight: the edge must
    // not creep past the rule boundary. 5.4065 was the first proposed value.
    report("60m 5.4065 is OUTSIDE the band (rule edge is 5406.4 kHz)",
           bandIs(5.4065, "GEN"));

    // The band's default tune frequency must land inside its own band, or the
    // band button tunes somewhere frequency->band lookup calls "GEN".
    report("60m default 5.357 is inside 60m",
           bandIs(5.357, "60m"));

    // ── Every band's own edges and default are self-consistent ───────────────
    // Cheap invariant over the whole table so the next edit to any row cannot
    // repeat the #4723 shape of defect: edges ordered, default inside them, and
    // no two bands overlapping (which would make bandForFrequency() order-
    // dependent — it returns the first match).
    bool ordered = true, defaultsInside = true, disjoint = true;
    for (int i = 0; i < kBandCount; ++i) {
        const BandDef& b = kBands[i];
        if (!(b.lowMhz < b.highMhz))
            ordered = false;
        if (!(b.defaultFreqMhz >= b.lowMhz && b.defaultFreqMhz <= b.highMhz))
            defaultsInside = false;
        for (int j = i + 1; j < kBandCount; ++j) {
            if (kBands[i].lowMhz <= kBands[j].highMhz
                && kBands[j].lowMhz <= kBands[i].highMhz)
                disjoint = false;
        }
    }
    report("every band has lowMhz < highMhz", ordered);
    report("every band's default tune frequency is inside its own edges",
           defaultsInside);
    report("no two bands overlap", disjoint);

    // ── A couple of ordinary bands, so an edit that guts the table is caught ──
    report("14.225 resolves to 20m", bandIs(14.225, "20m"));
    report("7.200 resolves to 40m", bandIs(7.200, "40m"));
    report("5.000 (WWV, between bands) resolves to GEN", bandIs(5.000, "GEN"));

    if (g_failed == 0) {
        std::printf("\nAll %d band-edge tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d band-edge tests failed.\n", g_failed, g_total);
    return 1;
}
