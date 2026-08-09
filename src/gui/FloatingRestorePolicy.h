#pragma once

namespace AetherSDR {

// Settings keys backing the floating-panadapter crash-loop guard (#4617).
//
// "FloatingPanIds" is the list of pans that were floated when the session
// ended; the post-connect layout restore replays it. "FloatingPanRestorePending"
// is armed immediately before every float — interactive or replayed — and
// cleared once the new window has survived kFloatingRestoreSettleMs, so finding
// it armed at startup means the previous process died inside the float.
inline constexpr char kFloatingPanIdsKey[] = "FloatingPanIds";
inline constexpr char kFloatingRestorePendingKey[] = "FloatingPanRestorePending";

enum class FloatingRestoreAction {
    // No evidence of a previous failure — replay the saved IDs as before.
    Replay,
    // The previous process armed the marker and never cleared it, and it left
    // pan IDs behind. Those IDs are what it was floating when it died, so
    // replaying them reproduces the crash. Forget them and come up docked.
    DropSavedIds,
    // Marker armed but nothing saved to replay (the previous process died
    // after the float had already been undone, or the IDs were cleared by
    // hand). Nothing to protect the user from; just retire the marker.
    ClearStaleMarker,
};

// Decide what a starting session should do with the persisted float state.
//
// This exists because a crash inside floatPanadapter() used to be terminal
// rather than merely annoying: saveFloatingState() commits the pan ID *before*
// the reparent + GPU re-initialize that historically took the process down on
// marginal D3D11 drivers (#4319, #4091, #4617), and restoreFloatingState()
// replayed the saved list unconditionally on every connect. The result was a
// boot loop with no in-app escape — the user had to hand-edit the settings
// store to get their radio back. One armed-across-the-crash marker turns that
// into a single lost pop-out.
//
// Deliberately conservative: it only discards state when a *previous* process
// left the marker armed. A session that is merely slow to settle keeps its
// layout, because the marker is evaluated once at construction, before this
// session's own floats can arm it.
constexpr FloatingRestoreAction evaluateFloatingRestore(bool haveSavedIds,
                                                        bool restorePending)
{
    if (!restorePending) {
        return FloatingRestoreAction::Replay;
    }
    return haveSavedIds ? FloatingRestoreAction::DropSavedIds
                        : FloatingRestoreAction::ClearStaleMarker;
}

} // namespace AetherSDR
