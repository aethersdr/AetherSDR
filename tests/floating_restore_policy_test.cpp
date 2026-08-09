// Pins the floating-panadapter crash-loop guard (#4617).
//
// Popping out a panadapter commits the pan ID to "FloatingPanIds" before the
// reparent + GPU re-initialize that has historically killed the process on
// marginal D3D11 drivers (#4319 / #4091). The post-connect restore then
// replayed that list unconditionally, so a single pop-out crash became a boot
// loop with no in-app escape. evaluateFloatingRestore() is the decision that
// breaks it: one marker armed across the crash, one discarded pop-out.

#include "gui/FloatingRestorePolicy.h"

#include <cstdio>

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "floating_restore_policy_test: %s\n", message);
    return 1;
}

int testHealthySessionReplays()
{
    using namespace AetherSDR;

    // The overwhelmingly common case: pans were floated, the session that
    // floated them shut down cleanly and retired the marker. Restoring the
    // layout is the whole point of persisting it, so nothing may interfere.
    if (evaluateFloatingRestore(true, false) != FloatingRestoreAction::Replay) {
        return fail("a cleanly closed session must get its floating pans back");
    }

    // Nothing saved and no marker — restoreFloatingState() has nothing to do,
    // and must not be told to clear state it never wrote.
    if (evaluateFloatingRestore(false, false) != FloatingRestoreAction::Replay) {
        return fail("an empty saved list is not evidence of a failure");
    }
    return 0;
}

int testCrashedSessionStartsDocked()
{
    using namespace AetherSDR;

    // The #4617 case. The marker survived, and so did the pan IDs the previous
    // process was floating when it died — those exact IDs are what the replay
    // would feed straight back into floatPanadapter(). Drop them.
    if (evaluateFloatingRestore(true, true) != FloatingRestoreAction::DropSavedIds) {
        return fail("a session that died floating must not replay the same IDs");
    }
    return 0;
}

int testStaleMarkerIsRetiredNotEscalated()
{
    using namespace AetherSDR;

    // Marker armed but nothing left to replay — e.g. the pan was docked again
    // before the process died, or the IDs were cleared by hand as the #4617
    // workaround. There is no crash to protect anyone from here, and treating
    // it as one would leave the marker armed forever, so this must retire the
    // marker WITHOUT being confused for the drop case.
    if (evaluateFloatingRestore(false, true)
        != FloatingRestoreAction::ClearStaleMarker) {
        return fail("an armed marker with nothing saved must just be retired");
    }
    return 0;
}

int testGuardIsSingleUse()
{
    using namespace AetherSDR;

    // Once the marker is retired, the very next evaluation of the same saved
    // IDs must replay them. The guard costs the user one pop-out, not their
    // ability to ever pop out again — a sticky guard would be a worse bug than
    // the one it fixes.
    if (evaluateFloatingRestore(true, true) != FloatingRestoreAction::DropSavedIds) {
        return fail("first evaluation after a crash must drop");
    }
    if (evaluateFloatingRestore(true, false) != FloatingRestoreAction::Replay) {
        return fail("the guard must not persist past the run that consumed it");
    }
    return 0;
}

} // namespace

int main()
{
    if (const int rc = testHealthySessionReplays()) return rc;
    if (const int rc = testCrashedSessionStartsDocked()) return rc;
    if (const int rc = testStaleMarkerIsRetiredNotEscalated()) return rc;
    if (const int rc = testGuardIsSingleUse()) return rc;
    std::printf("floating_restore_policy_test: all cases passed\n");
    return 0;
}
