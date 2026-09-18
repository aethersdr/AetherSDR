// HL2 per-receiver index-space map. The property under test is NOT that the
// lookups work — it is that the four index spaces stay INDEPENDENT, because the
// bug this type exists to prevent is code that derives one from another while
// they happen to be equal and breaks when they stop being.
//
// docs/HERMES.md §12.5, consolidated-backlog item 20.

#include "core/backends/hl2/Hl2Receivers.h"

#include <algorithm>
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

    // ---- append and remove: the two index spaces diverge on purpose ----
    //
    // This is the scenario the whole type exists for. The gateware streams
    // `numRx` CONTIGUOUS receivers and a receiver's DDC index IS its slot in the
    // interleaved EP6 round, so closing the middle one must renumber the
    // hardware indices. The operator's panes must NOT be renumbered with them —
    // closing panadapter 2 of 3 cannot turn their third panadapter into their
    // second one.
    {
        Hl2ReceiverMap m;
        m.reset(3);
        const QString pan2 = m.byUi(2)->panId;

        check(m.remove(1), "removing the middle receiver succeeds");
        check(m.size() == 2, "two receivers left");

        // DDC indices closed the gap...
        check(m.byDdc(0) != nullptr && m.byDdc(1) != nullptr,
              "DDC indices stay contiguous from zero");
        check(m.byDdc(2) == nullptr, "no DDC 2 remains");

        // ...but the UI numbers did NOT.
        check(m.byUi(0) != nullptr, "UI 0 survives");
        check(m.byUi(1) == nullptr, "UI 1 is gone — that pane was closed");
        check(m.byUi(2) != nullptr, "UI 2 survives and is STILL called 2");
        check(m.byUi(2)->ddcIndex == 1, "the surviving pane moved to DDC 1");
        check(m.byUi(2)->panId == pan2, "its pan id is unchanged");
        check(m.byPanId(pan2)->ddcIndex == 1, "and resolves to its new DDC");

        // Appending REUSES the retired UI number — the lowest one free.
        //
        // This assertion previously demanded the opposite ("the new pane is 3,
        // NOT the retired 1"), justified as avoiding two panes sharing an
        // identity. That was wrong: the retired pane does not exist, so nothing
        // collides. And the UI number IS the seam's slice id, whose space is
        // bounded by the radio's slice capacity — a monotonic counter handed out
        // slice id 4 on a 4-slice radio and the client refused it as over
        // capacity, which is what "Slice capacity is full" looked like after
        // closing three of four receivers.
        const int ddc = m.append();
        check(ddc == 2, "the new receiver takes the next free DDC index");
        check(m.byDdc(2)->uiNumber == 1, "the new pane REUSES the retired UI 1");
        check(m.byUi(1) != nullptr, "UI 1 is live again");
        check(m.byDdc(2)->dspChannel == -1, "a new receiver has no DSP channel yet");
        check(m.byDdc(2)->panId == hl2PanId(1), "its pan id follows its UI number");
        // The live pane keeps its own number — reuse must not disturb it.
        check(m.byUi(2)->ddcIndex == 1, "the surviving pane is still UI 2 on DDC 1");

        // UI numbers must stay inside 0..size-1, which is what the slice-id
        // space above the seam assumes.
        int highest = 0;
        for (const auto& r : m.all())
            highest = std::max(highest, r.uiNumber);
        check(highest < m.size(), "no UI number exceeds the receiver count");

        check(!m.remove(9), "removing an index that does not exist fails");
        check(m.size() == 3, "a failed remove changes nothing");
    }

    // ---- a stored ROLE index survives the renumber ----
    //
    // Transmit ownership and the active slice are remembered as "the receiver at
    // DDC n". remove() renumbers, so those stored indices must move with it, and
    // the case that bites is the middle one below: closing DDC 0 of three with
    // transmit on DDC 2 leaves transmit naming index 2, which is now a DIFFERENT
    // receiver — and nothing reads a TX NCO back to contradict it.
    {
        // Role BEFORE the removal: untouched.
        check(hl2RoleAfterRemove(0, 1) == 0, "a role below the removed index is unchanged");
        check(hl2RoleAfterRemove(1, 2) == 1, "role 1, removed 2 -> still 1");

        // Role AFTER the removal: shifts down by one.
        check(hl2RoleAfterRemove(2, 0) == 1, "role 2, removed 0 -> 1 (the renumber)");
        check(hl2RoleAfterRemove(3, 1) == 2, "role 3, removed 1 -> 2");
        check(hl2RoleAfterRemove(1, 0) == 0, "role 1, removed 0 -> 0");

        // Role ON the removed receiver: gone. -1 rather than a guess, so the
        // caller has to choose a new home explicitly.
        check(hl2RoleAfterRemove(2, 2) == -1, "a role on the removed receiver reports gone");
        check(hl2RoleAfterRemove(0, 0) == -1, "including receiver 0");

        // Cross-check the rule against the map actually renumbering: put a role
        // on the LAST of three, close the first, and the role must name the same
        // receiver afterwards.
        Hl2ReceiverMap m;
        m.reset(3);
        const int roleUi = m.byDdc(2)->uiNumber;      // identity of the role's receiver
        const int role = hl2RoleAfterRemove(2, 0);
        check(m.remove(0), "close DDC 0 of three");
        check(m.byDdc(role) != nullptr, "the shifted role index still resolves");
        check(m.byDdc(role)->uiNumber == roleUi,
              "and resolves to the SAME receiver it named before the removal");
    }

    // ---- removing the last receiver leaves an empty map ----
    {
        Hl2ReceiverMap m;
        m.reset(1);
        check(m.remove(0), "the only receiver can be removed from the map");
        check(m.empty(), "map is empty");
        m.reset(1);
        check(m.byDdc(0)->uiNumber == 0, "reset numbers from 0");
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
