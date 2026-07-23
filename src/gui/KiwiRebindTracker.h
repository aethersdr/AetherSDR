#pragma once

#include <QHash>
#include <QSet>
#include <QString>

namespace AetherSDR {

// Policy for retaining a KiwiSDR RX replacement across the slice remove->re-add
// that a FLEX band-stack recall performs. With band_persistence a band recall
// does NOT retune the slice: the radio DROPS it (in_use=0) and RE-CREATES it
// (same id, new band) a moment later, so a naive "tear the Kiwi down on
// sliceRemoved" reverts audio to the Flex antenna on every band change (#4158).
//
// This is a pure state machine — no widgets, no timers, no side effects — so
// MainWindow owns the side effects (disconnect / re-establish / mute) and this
// decides WHEN. Kept testable on its own because slice-id identity is subtle:
//   * Flex recycles slice ids, so id + elapsed time is NOT identity — a re-bind
//     must be gated on positive band-recall intent (noteBandRecall) or an
//     unrelated close+create would steal the Kiwi onto the wrong slice;
//   * the reconnect stale-slice prune emits sliceRemoved(oldId) while that id
//     already belongs to a live new-session slice, so a non-live removal must
//     never defer/tear down;
//   * repeated remove->re-add->remove within the grace window must be
//     generation-safe so an older expiry can't consume newer pending state.
// Unit-tested in kiwi_rebind_tracker_test.
class KiwiRebindTracker {
public:
    // A band recall was initiated by us on this pan; the slice the radio
    // re-creates on it should re-bind rather than tear down. One-shot per pan
    // (consumed by onSliceAdded on a match); MainWindow also expires it on a
    // timer so a recall that yields no re-bindable slice can't leave it stale.
    void noteBandRecall(const QString& panId) { m_bandRecallPans.insert(panId); }
    void clearBandRecall(const QString& panId) { m_bandRecallPans.remove(panId); }

    struct RemoveAction {
        enum Kind {
            Ignore,        // nothing Kiwi-related to do
            TeardownNow,   // finalize immediately (non-Kiwi live close, or the
                           // reconnect stale-prune — pre-existing behavior)
            Defer,         // a Kiwi live-removal: hold across the grace window
        };
        Kind     kind{Ignore};
        quint64  generation{0};   // valid when kind == Defer
    };

    // A slice was removed. `live` is false for the reconnect stale-prune (the id
    // still resolves to a live slice) — never defer then. `kiwiProfile` is empty
    // unless the removed slice carried a Kiwi replacement.
    RemoveAction onSliceRemoved(int sliceId, bool live, const QString& kiwiProfile)
    {
        if (live && !kiwiProfile.isEmpty()) {
            const quint64 generation = ++m_generation;
            m_pending.insert(sliceId, {kiwiProfile, generation});
            return {RemoveAction::Defer, generation};
        }
        return {RemoveAction::TeardownNow, 0};
    }

    // A slice was (re-)added. Returns the profile to re-bind, or an empty string
    // for "do nothing". Re-binds only when a rebind is pending for this id AND
    // this pan actually just did a band recall — so a plain id reuse leaves the
    // pending entry to time out (finalizing the original teardown) instead of
    // hijacking the Kiwi onto an unrelated slice.
    QString onSliceAdded(int sliceId, const QString& panId)
    {
        const auto it = m_pending.constFind(sliceId);
        if (it == m_pending.constEnd()) {
            return QString();
        }
        if (!m_bandRecallPans.contains(panId)) {
            return QString();
        }
        const QString profileId = it->profileId;
        m_pending.remove(sliceId);
        m_bandRecallPans.remove(panId);
        return profileId;
    }

    // The grace window elapsed for a deferred removal. Returns true only if this
    // exact (sliceId, generation) is still the pending entry — an older timer
    // whose entry was superseded by a newer removal returns false, so it can't
    // finalize a teardown that no longer applies. On true, the entry is dropped
    // and MainWindow performs the real teardown.
    bool onGraceExpired(int sliceId, quint64 generation)
    {
        const auto it = m_pending.constFind(sliceId);
        if (it == m_pending.constEnd() || it->generation != generation) {
            return false;
        }
        m_pending.remove(sliceId);
        return true;
    }

    // Test/inspection helpers.
    bool hasPending(int sliceId) const { return m_pending.contains(sliceId); }
    bool bandRecallPending(const QString& panId) const
    {
        return m_bandRecallPans.contains(panId);
    }

private:
    struct Pending {
        QString profileId;
        quint64 generation{0};
    };
    QHash<int, Pending> m_pending;        // removed Kiwi slice id -> pending rebind
    QSet<QString>       m_bandRecallPans;  // pans with an in-flight band recall
    quint64             m_generation{0};   // monotonic; disambiguates re-removals
};

}  // namespace AetherSDR
