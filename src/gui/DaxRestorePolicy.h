#pragma once

#include <QChar>
#include <QString>
#include <QStringList>

// DaxRestorePolicy — when the last-session per-slice DAX restore (#1221) may
// apply, and which persisted keys a quit may prune (#4558).
//
// Background (#4558): the restore reads `DaxChannel_Slice<letter>` keyed by
// SLICE LIST POSITION (A = index 0, ...) because radio-assigned slice ids are
// not stable across sessions. That is sound for the launch-time restore it was
// designed for and unsound for anything later: a band-stack switch destroys and
// recreates its slice, and the recreate re-enters the list at the TAIL, so with
// another slice alive it resolves the OTHER position's key and re-assigns that
// stale channel to itself 300 ms after the add — stealing the surviving slice's
// live DAX channel, unannounced. Confining the restore to the initial
// post-connect enumeration removes the mis-key's only opportunity.
//
// Pulled out of MainWindow as pure, header-only logic so both decisions can be
// unit-tested without a radio, an event loop, or a settings store — mirroring
// CenterLockRebindTracker / KiwiRebindTracker / SliceLinkPolicy, which cover
// the same live-vs-prune reasoning in the same two handlers.

namespace AetherSDR {

class DaxRestorePolicy {
public:
    // ── The post-connect restore window ────────────────────────────────────
    //
    // Opens on connect and closes on the first LIVE slice removal or a settle
    // timeout, whichever comes first. A mid-session recreate is ALWAYS preceded
    // by a removal, so the window is shut before the recreated slice's add can
    // consult it; the initial enumeration is adds-only, so it restores exactly
    // as it always did.

    // A connect completed. Reopens the window and starts a new generation.
    //
    // NOTE this fires on EVERY connect, not just the first of the session: an
    // auto-reconnect re-enumerates and legitimately needs the restore (its
    // slices were torn down). The corollary is that persisted values are
    // last-QUIT values, so a reconnect can write them over assignments the
    // operator changed live earlier in the session. That is pre-#4558 behavior,
    // deliberately preserved here; narrowing it further is a separate question
    // about whether #1221 should persist Flex-owned state client-side at all.
    void onConnected()
    {
        m_windowOpen = true;
        ++m_generation;
    }

    // The link dropped. The window cannot span a disconnect — the next connect
    // reopens it with a fresh generation.
    void onDisconnected() { m_windowOpen = false; }

    // Generation to hand to the settle timer, so a previous connect's pending
    // timeout cannot close the window this connect just opened.
    int generation() const { return m_generation; }

    // A sliceRemoved arrived. `liveRemoval` is the caller's live-vs-prune
    // discriminator (`m_radioModel.slice(id) == nullptr` — live removals emit
    // AFTER the slice leaves the model, so a non-null lookup means the id
    // already names a NEW-session slice and this is the post-reconnect stale
    // prune). Returns true when this call is what closed the window, so the
    // caller can log the transition once.
    //
    // The discriminator is one-directional: non-null proves "prune", null does
    // not prove "live". A stale prune of an id the new session never
    // re-enumerated, the foreign-owner takeover path, and the family-switch
    // drop all read as live. Every one of those fails toward closing the window
    // EARLY, and an early close can only skip a restore (see restoreAllowed()'s
    // sole consumer, whose false branch is log-and-skip) — never fire one.
    bool onSliceRemoved(bool liveRemoval)
    {
        if (!m_windowOpen || !liveRemoval) {
            return false;
        }
        m_windowOpen = false;
        return true;
    }

    // The settle timer armed for `generation` fired. Belt-and-braces for the
    // session that never removes a slice: without it, a slice the operator adds
    // hours later would still inherit a last-session key.
    void onSettleTimeout(int generation)
    {
        if (generation == m_generation) {
            m_windowOpen = false;
        }
    }

    // May a slice add apply its last-session DAX key?
    bool restoreAllowed() const { return m_windowOpen; }

    // ── The quit-time key space ────────────────────────────────────────────

    // Highest index the letter key space ever used. Bounds the historical
    // space, not any radio's slice count.
    static constexpr int kMaxSliceIndex = 'Z' - 'A';

    // The persisted key for a slice at list position `index`.
    static QString keyForIndex(int index)
    {
        return QStringLiteral("DaxChannel_Slice%1").arg(QChar('A' + index));
    }

    // Keys a quit should REMOVE beyond the live slices it just wrote.
    //
    // #4558's precondition is a key written by an earlier multi-slice quit that
    // no later quit could clean, because the writer's loop was bounded by the
    // slices open at quit — so `DaxChannel_SliceB` from an old two-slice
    // session survived every single-slice quit and stayed armed forever.
    // Pruning the tail at quit makes an affected config self-heal.
    //
    // Both guards are load-bearing, and each was a data-loss bug in review:
    //
    //  - `connected`: a radio-less quit (never connected, failed connect, or
    //    disconnected first) has an empty list that says nothing about the
    //    operator's saved layout. Erasing there loses the restore data on every
    //    radio-less launch. (#4572 review 1)
    //  - `liveSliceCount > 0`: "connected" alone does NOT mean the list is an
    //    authoritative enumeration. RadioModel::onConnected() clears m_slices
    //    (stageSessionModelsForReconnect) BEFORE emitting
    //    connectionStateChanged(true), so the list is empty for the whole
    //    pre-enumeration window, and stays empty for a connected session that
    //    owns no slices — GUI registration rejected (#4563), every slice owned
    //    by another client, a stalled WAN/SmartLink status trickle. An empty
    //    list in those states is absence of evidence, not an authoritative
    //    zero. (#4572 review 2)
    //
    // Refusing to prune at zero costs nothing: a session that owns no slices
    // cannot have written a stale key, and the next quit that does own slices
    // prunes the tail anyway.
    static QStringList staleKeysToPrune(bool connected, int liveSliceCount)
    {
        QStringList keys;
        if (!connected || liveSliceCount <= 0) {
            return keys;
        }
        for (int i = liveSliceCount; i <= kMaxSliceIndex; ++i) {
            keys << keyForIndex(i);
        }
        return keys;
    }

private:
    bool m_windowOpen{false};
    int m_generation{0};
};

}  // namespace AetherSDR
