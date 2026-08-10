#pragma once

namespace AetherSDR {

// Settings keys backing the floating-panadapter crash-loop guard (#4617).
//
// "FloatingPanIds" is the list of pans that were floated when the session
// ended; the post-connect layout restore replays it. "FloatingPanRestorePending"
// is armed and committed to disk immediately before every float — interactive
// or replayed, and before the GPU teardown/reparent, not after — then cleared
// once the new window has survived kFloatingRestoreSettleMs, so finding it
// armed at startup means the previous process died inside the float.
inline constexpr char kFloatingPanIdsKey[] = "FloatingPanIds";
inline constexpr char kFloatingRestorePendingKey[] = "FloatingPanRestorePending";

// The settings store's boolean spelling (AGENTS.md, "Settings Persistence":
// booleans are stored as the strings "True"/"False"). Named here rather than
// written inline so the marker cannot drift back to a C++ bool, which
// AppSettings stringifies as "true" — that round-trips through .toBool(), but
// reads as false to the documented
// `value(k, "False").toString() == "True"` idiom, and silent-false is the
// wrong direction for a crash-loop guard to fail in.
inline constexpr char kSettingsTrue[] = "True";
inline constexpr char kSettingsFalse[] = "False";

enum class FloatingRestoreAction {
    // No evidence of a previous failure — replay the saved IDs as before.
    Replay,
    // The previous process armed the marker and never cleared it, and it left
    // pan IDs behind. One of those IDs is what it was floating when it died,
    // so replaying the list reproduces the crash. Forget them and come up
    // docked.
    //
    // The drop is deliberately coarse: the marker is a bare flag, so *all*
    // saved IDs go, not just the one in flight. Floating a second pan inside
    // the settle window therefore puts the first one at risk too, and a crash
    // from an unrelated cause within that window costs the whole pop-out
    // layout. Both are one pop-out's worth of loss on a guard that is
    // single-use by construction, and narrowing it would mean persisting the
    // in-flight ID — more state to keep consistent across the exact code path
    // that is known to die mid-write.
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

// What the starting session must persist for a given action.
//
// Split out of the caller's switch so the *writes* are pinned by the test, not
// only the decision. The mutation that matters is deleting the marker-clearing
// write: the guard would then stay armed forever and every subsequent launch
// would drop the layout — a worse bug than the one it fixes — while
// evaluateFloatingRestore() still returned the right answer and the suite
// still passed. Routing the writes through a pure function makes that mutation
// fail a test instead of merely contradicting a comment.
struct FloatingRestoreWrites {
    bool clearSavedIds;  // blank kFloatingPanIdsKey
    bool clearMarker;    // write kFloatingRestorePendingKey = kSettingsFalse
};

constexpr FloatingRestoreWrites floatingRestoreWrites(FloatingRestoreAction action)
{
    switch (action) {
    case FloatingRestoreAction::DropSavedIds:
        return {true, true};
    case FloatingRestoreAction::ClearStaleMarker:
        return {false, true};
    case FloatingRestoreAction::Replay:
        break;
    }
    // Replay is the untouched-state case: a healthy session must not write to
    // the store just because it looked at it.
    return {false, false};
}

} // namespace AetherSDR
