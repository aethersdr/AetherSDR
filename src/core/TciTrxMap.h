#pragma once

#include <QHash>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Stable TCI receiver numbering (#4567).
//
// Wire receiver indexes used to be the slice's current position in
// RadioModel::slices() (the #2145 policy) — an insertion-ordered list that a
// band-stack destroy/recreate reorders: the recreated slice re-enters at the
// tail, so every surviving slice silently changes receiver number under a
// live TCI client, and the client's audio/control follow the wrong slice.
//
// This map pins sliceId → trx for the life of a binding instead:
//  - acquire() on sliceAdded reuses an existing binding (a band-stack
//    recreate keeps the same Flex slice id, so the recreated slice reclaims
//    its old receiver number and surviving slices never move) or assigns the
//    lowest free trx (keeping indexes dense from 0, which the TCI wire
//    contract requires — raw Flex slice ids are sparse and MultiFlex can
//    start them above 0, so they are NOT usable as receiver numbers).
//  - release() is deferred by the owner past the band-change settle window
//    (see TciServer's sliceRemoved handler), so only a genuine slice close
//    frees a number for reuse.
//
// Under a session with no mid-session destroy/recreate the assignment is
// identical to the positional policy (0,1,2… in creation order). Lookups on
// a slice with no binding fall back to the original positional logic
// (TciProtocol's statics), so pre-wiring windows behave exactly as before.
//
// Deliberately NOT part of TciRoutingState: that class excludes wire TRX
// indexes by design (TciRoutingState.h) — this class is the one owner of
// them. Not thread-affine; owned and driven by TciServer on its thread.
class TciTrxMap
{
public:
    // Returns the trx bound to sliceId, binding the lowest free trx first if
    // none exists. Idempotent for an existing binding.
    int acquire(int sliceId);

    // Drops sliceId's binding (a later acquire may reuse the freed trx).
    void release(int sliceId);

    // Drops every binding (disconnect: slices die with the connection).
    void clear();

    // Map-first lookups; positional fallback when the slice has no binding.
    int trxForSlice(RadioModel* model, const SliceModel* slice) const;
    SliceModel* sliceForTrx(RadioModel* model, int trx) const;

    // sliceForTrx() for paths that key the radio (#4547): identical, except an
    // UNBOUND trx that resolves to nothing returns nullptr instead of falling
    // back to the first slice. The bound path is already fail-closed, so this
    // differs only in that last step — but it has to exist, because the PTT
    // path must not be the one caller still resolving positionally. If it went
    // through TciProtocol's statics it would ignore these bindings entirely,
    // and a band-stack recreate would key the slice that happens to sit at the
    // requested index — #4567 on the transmit path, where being wrong puts RF
    // on the wrong band and antenna.
    SliceModel* sliceForTrxStrict(RadioModel* model, int trx) const;

    // TRX index of the current TX slice, or -1 when none is marked.
    int txSliceTrxOrNone(RadioModel* model) const;

    // True when some live slice currently maps to `trx` (see TciServer's
    // #4161 close-vs-recreate discrimination).
    bool trxHasLiveSlice(RadioModel* model, int trx) const;

    // Advertised receiver count: 1 + the highest trx in use (bound or live),
    // never below 1 — a transient hole must not advertise a count below an
    // index a client is actively using.
    int trxCount(RadioModel* model) const;

private:
    // The shared bound lookup behind both sliceForTrx variants. `bound` says
    // whether some binding claims this trx at all, which is what distinguishes
    // "held through the settle window, answer nullptr" from "never bound, let
    // the caller pick its fallback" — the return value alone cannot.
    SliceModel* boundSliceForTrx(RadioModel* model, int trx, bool& bound) const;

    QHash<int, int> m_trxBySliceId;
};

} // namespace AetherSDR
