// HL2 IO-board push scheduling policy. Pure — no Qt, no aethercore, no radio.
//
// Every defect found in this feature during review was in the scheduling, not
// the encoder: a guard present on one edge of the throttle and absent on the
// other, an armed timer surviving a disconnect, a band change coalesced as
// though it were ordinary frequency drift. These checks pin the conditions so
// those cannot come back silently.

#include "core/backends/hl2/Hl2IoBoardPolicy.h"

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

// Named arguments at the call site: ioBoardAction(connected, keyed,
// throttleActive, bandChanged) is four bools, and a transposed pair would
// otherwise still compile and still look right.
static IoBoardAction act(bool connected, bool keyed, bool throttled, bool bandChanged)
{
    return ioBoardAction(connected, keyed, throttled, bandChanged);
}

int main()
{
    // ---- disconnected wins over everything ----
    {
        // The consequence outlives the session: a bank queued while down is not
        // discarded by start()/stop() and becomes the FIRST thing the next
        // connect sends, pointing an amplifier at the previous band.
        check(act(false, false, false, false) == IoBoardAction::DropDisconnected,
              "disconnected and idle drops");
        check(act(false, false, false, true) == IoBoardAction::DropDisconnected,
              "a band change while disconnected still drops");
        check(act(false, true, true, true) == IoBoardAction::DropDisconnected,
              "disconnected outranks every other condition");

        // THE REGRESSION. The original code guarded the trailing edge of the
        // throttle and not the leading one, so a tune while disconnected with
        // an idle timer queued five banks. That is this exact combination.
        check(act(false, false, /*throttled=*/false, /*bandChanged=*/true)
                  == IoBoardAction::DropDisconnected,
              "leading edge while disconnected drops (the reviewed regression)");
    }

    // ---- keyed defers, and is never overridden by a band change ----
    {
        // Switching a band relay under RF burns its contacts. A band change is
        // the one thing that otherwise beats the throttle, so it is the case
        // most likely to be let through by a careless edit.
        check(act(true, true, false, false) == IoBoardAction::DeferKeyed,
              "keyed defers");
        check(act(true, true, false, true) == IoBoardAction::DeferKeyed,
              "keyed defers EVEN on a band change — relay under RF");
        check(act(true, true, true, true) == IoBoardAction::DeferKeyed,
              "keyed defers regardless of the throttle");
    }

    // ---- the throttle coalesces same-band movement only ----
    {
        check(act(true, false, true, false) == IoBoardAction::Coalesce,
              "same-band movement inside the cooldown coalesces");
        check(act(true, false, false, false) == IoBoardAction::Send,
              "same-band movement with an idle throttle sends");
    }

    // ---- a band change beats the throttle ----
    {
        // applyBandFilter moves the physical filter relay immediately. Holding
        // the amplifier back for the cooldown leaves the two disagreeing, and
        // keying in that window is the hazard.
        check(act(true, false, true, true) == IoBoardAction::Send,
              "a band change sends on the leading edge even mid-cooldown");
        check(act(true, false, false, true) == IoBoardAction::Send,
              "a band change with an idle throttle sends");
    }

    // ---- exhaustive: every combination has exactly one defined outcome ----
    {
        int sends = 0, coalesces = 0, defers = 0, drops = 0;
        for (int i = 0; i < 16; ++i) {
            const bool c = i & 1, k = i & 2, t = i & 4, b = i & 8;
            switch (act(c, k, t, b)) {
            case IoBoardAction::Send:             ++sends;     break;
            case IoBoardAction::Coalesce:         ++coalesces; break;
            case IoBoardAction::DeferKeyed:       ++defers;    break;
            case IoBoardAction::DropDisconnected: ++drops;     break;
            }
        }
        check(drops == 8, "half of all states are disconnected, and all drop");
        check(defers == 4, "connected+keyed always defers");
        check(coalesces == 1, "only connected, unkeyed, throttled, same-band coalesces");
        check(sends == 3, "the remaining connected+unkeyed states send");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_io_board_policy_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
