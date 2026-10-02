#pragma once

#include <QChar>
#include <QString>
#include <QStringList>

// When the last-session per-slice DAX restore (#1221) may apply, and which keys
// a quit may prune (#4558). `DaxChannel_Slice<letter>` is keyed by slice LIST
// POSITION (ids are not stable across sessions). Only the initial post-connect
// enumeration is safe: a band-stack switch recreates its slice at the list tail,
// so a later restore would read another position's key and steal that slice's
// live DAX channel. Pure, so it is unit-tested like KiwiRebindTracker.

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

    // A sliceRemoved arrived. `liveRemoval` is `m_radioModel.slice(id) ==
    // nullptr` (live removals emit after the slice leaves the model; non-null
    // means a post-reconnect stale prune). Returns true when this call closed
    // the window. Non-null proves "prune" but null does not prove "live"; every
    // misread closes the window early, which can only skip a restore, never fire
    // one.
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

    // Keys a quit should REMOVE beyond the live slices it just wrote, so stale
    // tail keys from an earlier larger session get pruned (#4558).
    // `connected`: a radio-less quit's empty list says nothing about the saved
    // layout. `liveSliceCount > 0`: connected is not authoritative either —
    // onConnected() clears m_slices before connectionStateChanged(true), and a
    // session may own no slices. A session with no slices wrote no stale key.
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
