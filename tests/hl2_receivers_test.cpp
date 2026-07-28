// HL2 per-receiver index-space map. The property under test is NOT that the
// lookups work — it is that the four index spaces stay INDEPENDENT, because the
// bug this type exists to prevent is code that derives one from another while
// they happen to be equal and breaks when they stop being.
//
// HERMES.md §12.5, consolidated-backlog item 20.

#include "core/backends/hl2/Hl2Receivers.h"

#include <cstdio>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main()
{
    // ---- pan-id round trip ----
    {
        check(hl2PanId(0) == QStringLiteral("hl2-0"), "receiver 0 has a SUFFIXED pan id");
        check(hl2PanId(3) == QStringLiteral("hl2-3"), "receiver 3 pan id");
        check(hl2PanNumber(hl2PanId(2)) == 2, "pan id round-trips");

        // A pan id we did not issue must NOT resolve. Falling back to 0 would
        // point every unrecognised pan control at the first receiver — a slider
        // that moves the wrong panadapter rather than doing nothing.
        check(!hl2PanNumber(QStringLiteral("hl2")).has_value(), "bare 'hl2' is not a pan id");
        check(!hl2PanNumber(QStringLiteral("flex-0")).has_value(), "another family's pan id");
        check(!hl2PanNumber(QStringLiteral("hl2-")).has_value(), "empty number");
        check(!hl2PanNumber(QStringLiteral("hl2-x")).has_value(), "non-numeric");
        check(!hl2PanNumber(QStringLiteral("hl2--1")).has_value(), "negative");
        check(!hl2PanNumber(QString()).has_value(), "empty string");
    }

    // ---- reset builds contiguous DDC/UI, and leaves the rest UNKNOWN ----
    {
        Hl2ReceiverMap m;
        m.reset(4);
        check(m.size() == 4, "four receivers");
        for (int i = 0; i < 4; ++i) {
            const auto* r = m.byDdc(i);
            check(r != nullptr, "receiver found by DDC index");
            check(r && r->ddcIndex == i && r->uiNumber == i, "DDC and UI start contiguous");
            // The whole point: these are NOT seeded to `i`.
            check(r && r->dspChannel == -1, "dspChannel starts unknown, not derived from DDC");
            check(r && r->analyzerId == -1, "analyzerId starts unknown, not derived from DDC");
        }
        check(m.byDdc(4) == nullptr, "an index past the end does not resolve");
        check(m.byDdc(-1) == nullptr, "a negative index does not resolve");
        // -1 means "not open yet" and must not match every unopened receiver.
        check(m.byDspChannel(-1) == nullptr, "unopened channels are not searchable");
    }

    // ---- the spaces really are independent ----
    //
    // This is the scenario the type is for. WDSP channel ids come from a
    // process-wide pool of 32 shared with the transmit chain, so after a TX
    // channel has come and gone, receiver 0 is routinely NOT channel 0. A
    // lookup that assumed dspChannel == ddcIndex would silently address the
    // wrong receiver's DSP.
    {
        Hl2ReceiverMap m;
        m.reset(4);
        const int channels[4] = {7, 3, 11, 5};    // as a shared pool would hand them out
        const int analyzers[4] = {2, 9, 0, 4};
        for (int i = 0; i < 4; ++i) {
            auto* r = m.mutableByDdc(i);
            check(r != nullptr, "mutable lookup by DDC");
            if (!r) continue;
            r->dspChannel = channels[i];
            r->analyzerId = analyzers[i];
        }

        for (int i = 0; i < 4; ++i) {
            const auto* byChan = m.byDspChannel(channels[i]);
            check(byChan != nullptr && byChan->ddcIndex == i,
                  "a WDSP channel resolves to its own receiver, not to channel==ddc");
            const auto* byPan = m.byPanId(hl2PanId(i));
            check(byPan != nullptr && byPan->ddcIndex == i, "pan id resolves to its receiver");
        }
        // The mapping is genuinely not the identity — if it were, the checks
        // above would pass for the wrong reason.
        check(m.byDdc(0)->dspChannel != 0, "receiver 0 is not WDSP channel 0 here");
        check(m.byDspChannel(0) == nullptr, "channel 0 belongs to nobody in this layout");
        check(m.byDspChannel(3)->ddcIndex == 1, "channel 3 is receiver 1, not receiver 3");
        check(m.byDdc(2)->analyzerId == 0, "analyzer 0 belongs to receiver 2");
    }

    // ---- UI numbers can diverge from DDC indices ----
    //
    // PureSignal (backlog 23) consumes four receivers of which two are transmit
    // feedback with no slice at all, so ddcIndex and uiNumber stop being 1:1.
    {
        Hl2ReceiverMap m;
        m.reset(3);
        m.mutableByDdc(1)->uiNumber = 5;
        m.mutableByDdc(1)->panId = hl2PanId(5);
        check(m.byUi(5) != nullptr && m.byUi(5)->ddcIndex == 1, "UI 5 is DDC 1");
        check(m.byUi(1) == nullptr, "UI 1 no longer exists");
        check(m.byPanId(hl2PanId(5))->ddcIndex == 1, "pan id follows the UI number");
        check(m.byDdc(1)->ddcIndex == 1, "the DDC index is unchanged by the UI renumber");
    }

    // ---- degenerate sizes ----
    {
        Hl2ReceiverMap m;
        m.reset(0);
        check(m.empty() && m.byDdc(0) == nullptr, "zero receivers resolves nothing");
        m.reset(-1);
        check(m.empty(), "a negative count is not a huge allocation");
        m.reset(1);
        check(m.size() == 1 && m.byUi(0) != nullptr, "back to one receiver");
        m.clear();
        check(m.empty(), "clear empties the map");
    }

    if (g_failures == 0)
        std::printf("hl2_receivers_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
