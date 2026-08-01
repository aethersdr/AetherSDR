// Waterfall Black Level mode arithmetic (#4606) — header-only, pure logic.
//
// Both bugs pinned here shipped through a full green CI run, because the only
// coverage of this control was manual. They are:
//
// 1. WRITE-THROUGH. The capability gate coerced the stored mode to SW *and*
//    emitted the normal change signals, which reach SpectrumWidget's persisting
//    setter. One session on an HL2 permanently destroyed a Flex user's HW
//    preference. The gate must MASK intent, never rewrite it.
//
// 2. WIDGET OWNERSHIP. The Off/SW/HW cycle shares its button and slider with
//    KiwiSDR mode, where the button is a one-shot "Auto" and the slider is a
//    dBm floor over -260..29. Every caller of the cycle's apply function
//    happened to be Kiwi-guarded until a capability-change caller was added,
//    which then relabelled "Auto" as "SW".

#include "gui/AutoBlackMode.h"

#include <cstdio>

namespace AB = AetherSDR::AutoBlackMode;

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

int main()
{
    // ── Masking: intent survives, effect adapts. ───────────────────────────
    report("HW intent shows as SW on a radio without one",
           AB::effective(AB::kHardware, false) == AB::kSoftware);
    report("HW intent shows as HW on a radio with one",
           AB::effective(AB::kHardware, true) == AB::kHardware);
    report("masked HW resolves to SW, not Off",
           AB::effective(AB::kHardware, false) != AB::kOff);
    report("Off and SW are unaffected by the capability",
           AB::effective(AB::kOff, false) == AB::kOff
               && AB::effective(AB::kOff, true) == AB::kOff
               && AB::effective(AB::kSoftware, false) == AB::kSoftware
               && AB::effective(AB::kSoftware, true) == AB::kSoftware);

    // The regression that motivated option B, stated as the sequence that broke:
    // the operator picks HW on a Flex, runs a session on an HL2, and reconnects
    // the Flex. `intent` is the stored value and must be the SAME variable
    // throughout — masking may not write back to it.
    int intent = AB::kHardware;              // chose HW on a Flex
    const int onFlex = AB::effective(intent, /*available=*/true);
    const int onHl2 = AB::effective(intent, /*available=*/false);
    const int backOnFlex = AB::effective(intent, /*available=*/true);
    report("HW -> (HL2 session shows SW) -> HW again, intent never rewritten",
           onFlex == AB::kHardware
               && onHl2 == AB::kSoftware
               && backOnFlex == AB::kHardware
               && intent == AB::kHardware);

    // …and the one case where losing the stashed HW is correct: a click is a
    // real choice on the radio in front of the operator, so it replaces intent.
    intent = AB::nextOnClick(intent, /*available=*/false);
    report("a deliberate click on the gated radio does replace the intent",
           intent == AB::kOff
               && AB::effective(intent, /*available=*/true) == AB::kOff);

    // ── Reachable positions. ──────────────────────────────────────────────
    report("three positions when the radio computes a level",
           AB::modeCount(true) == 3);
    report("two positions when it does not",
           AB::modeCount(false) == 2);

    // ── The click cycle. ──────────────────────────────────────────────────
    report("Flex cycle is Off -> SW -> HW -> Off",
           AB::nextOnClick(AB::kOff, true) == AB::kSoftware
               && AB::nextOnClick(AB::kSoftware, true) == AB::kHardware
               && AB::nextOnClick(AB::kHardware, true) == AB::kOff);
    report("gated cycle is Off <-> SW and never reaches HW",
           AB::nextOnClick(AB::kOff, false) == AB::kSoftware
               && AB::nextOnClick(AB::kSoftware, false) == AB::kOff);
    // Advancing from the masked intent rather than from what is displayed would
    // give HW+1 % 2 == 1 == SW — the button would appear not to respond.
    report("clicking a masked HW advances to Off, not back to SW",
           AB::nextOnClick(AB::kHardware, false) == AB::kOff);
    bool neverHardware = true;
    int m = AB::kHardware;   // start from the worst case: a stashed HW
    for (int i = 0; i < 8; ++i) {
        m = AB::nextOnClick(m, false);
        if (m == AB::kHardware) neverHardware = false;
    }
    report("no sequence of clicks reaches HW on a radio without one",
           neverHardware);

    // ── Clamping. ─────────────────────────────────────────────────────────
    report("out-of-range intent clamps into 0..2",
           AB::clampIntent(-5) == AB::kOff && AB::clampIntent(99) == AB::kHardware);
    report("clamping keeps HW reachable — it must NOT clamp to the mode count",
           AB::clampIntent(AB::kHardware) == AB::kHardware);

    // ── The source flag the renderer and the radio see. ───────────────────
    report("effective source is intent AND capability",
           AB::effectiveRadioSide(true, true) == true
               && AB::effectiveRadioSide(true, false) == false
               && AB::effectiveRadioSide(false, true) == false
               && AB::effectiveRadioSide(false, false) == false);

    // ── Widget ownership — bug 2. ─────────────────────────────────────────
    report("the cycle owns the shared button/slider outside Kiwi mode",
           AB::ownsSharedWidgets(false) == true);
    report("Kiwi mode owns them instead, so the cycle must not write",
           AB::ownsSharedWidgets(true) == false);

    if (g_failed == 0) {
        std::printf("\nAll %d auto-black-mode tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d auto-black-mode tests failed.\n", g_failed, g_total);
    return 1;
}
