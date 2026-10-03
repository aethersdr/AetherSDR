#pragma once

#include <QHash>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Stable TCI receiver numbering (#4567): pins sliceId → trx for the life of a
// binding, so a band-stack destroy/recreate (which re-appends the slice to
// RadioModel::slices()) doesn't renumber surviving slices under a client.
//  - acquire() on sliceAdded reuses an existing binding (recreate keeps the
//    Flex slice id) or assigns the lowest free trx; TCI needs dense indexes
//    from 0, which raw Flex slice ids (sparse, MultiFlex may start above 0)
//    are not.
//  - release() is deferred by TciServer past the band-change settle window,
//    so only a genuine close frees a number.
// Unbound slices fall back to positional lookup (TciProtocol statics). The
// sole owner of wire TRX indexes (TciRoutingState excludes them). Not
// thread-affine; driven by TciServer on its thread.
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
