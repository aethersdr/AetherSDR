#include "TciRoutingState.h"

namespace AetherSDR
{

bool TciRoutingState::contains(const QVector<TciSliceEndpoint>& endpoints, int sliceId)
{
    for (const TciSliceEndpoint& endpoint : endpoints) {
        if (endpoint.sliceId == sliceId) {
            return true;
        }
    }
    return false;
}

bool TciRoutingState::operatedByAnotherClient(
    const QVector<TciSliceEndpoint>& endpoints, int sliceId)
{
    for (const TciSliceEndpoint& endpoint : endpoints) {
        if (endpoint.sliceId == sliceId) {
            return endpoint.operatedByAnotherClient;
        }
    }
    return false;
}

int TciRoutingState::currentTxSlice(const QVector<TciSliceEndpoint>& endpoints)
{
    for (const TciSliceEndpoint& endpoint : endpoints) {
        if (endpoint.isTx) {
            return endpoint.sliceId;
        }
    }
    return -1;
}

TciRoutingState::RouteDecision TciRoutingState::resolveVfoB(
    int rxSliceId, const QVector<TciSliceEndpoint>& endpoints)
{
    if (!contains(endpoints, rxSliceId)) {
        return {};
    }

    const int currentTx = currentTxSlice(endpoints);

    // A foreign TX slice that another client operates as its receiver is not this
    // receiver's VFO B: two WSJT-X instances each send `vfo:<trx>,1,<hz>` on band
    // change, and adopting it would retune the other instance's slice and the TX
    // frequency (#5193). Single-client cases still adopt (TX parked on an
    // unoperated slice, #1807), and a requested split still routes below. When the
    // requester's own slice holds TX, the Create/Promote path applies.
    const bool currentTxIsAnotherReceiver = currentTx >= 0 && currentTx != rxSliceId
        && operatedByAnotherClient(endpoints, currentTx);
    if (currentTxIsAnotherReceiver && !m_splitRequested) {
        // Records no new route (this is a per-frame decision). But any External bind
        // must be dropped: an External bind is always the live TX slice, which is now
        // another client's receiver, so resolvePttSlice() would hand it to the next
        // bare PTT. A TciCreated route is kept; resolvePttSlice() refuses it itself.
        if (m_owner == TxRouteOwner::External) {
            m_rxSliceId = -1;
            m_txSliceId = -1;
            m_owner = TxRouteOwner::None;
        }
        return { RouteAction::EchoOnly, -1, TxRouteOwner::None };
    }

    if (currentTx >= 0 && currentTx != rxSliceId && !currentTxIsAnotherReceiver) {
        // Always track the current RX slice, even when the external TX slice is
        // unchanged. removeSlice() keys off m_rxSliceId, so a stale value would
        // let the wrong slice's removal tear the route down (and miss the real
        // RX's removal).
        m_rxSliceId = rxSliceId;
        if (currentTx != m_txSliceId) {
            m_txSliceId = currentTx;
            m_owner = TxRouteOwner::External;
        }
        return { RouteAction::UseExisting, currentTx, m_owner };
    }

    if (m_txSliceId >= 0 && m_txSliceId != rxSliceId && contains(endpoints, m_txSliceId)
        && !operatedByAnotherClient(endpoints, m_txSliceId)) {
        m_rxSliceId = rxSliceId;
        return { RouteAction::PromoteExisting, m_txSliceId, m_owner };
    }

    // A non-TX slice may be an operator's independent receiver. Without an
    // explicit ownership signal, commandeering and retuning it is unsafe.
    // Note this also overwrites a TciCreated route whose slice the promote
    // above refused (another client now operates it): that slice becomes
    // untracked and split teardown will not `slice remove` it. Pre-existing;
    // the alternative, removing a slice another client operates, is worse.
    m_rxSliceId = rxSliceId;
    m_txSliceId = -1;
    m_owner = TxRouteOwner::None;
    return { RouteAction::Create, -1, TxRouteOwner::TciCreated };
}

int TciRoutingState::resolvePttSlice(int rxSliceId, const QVector<TciSliceEndpoint>& endpoints)
{
    if (!contains(endpoints, rxSliceId)) {
        return -1;
    }

    const int currentTx = currentTxSlice(endpoints);

    // A TX route answers this request only when the client is actually
    // operating one: it asked for split, or VFO B bound a route for exactly
    // this RX slice (the satellite case — an external controller selected the
    // TX slice and channel 1 adopted it). Previously this branch was
    // unconditional, and because a Flex always marks exactly one TX slice, it
    // fired on every request whose slice was not already TX — discarding the
    // requested trx on the common path, not an edge case, so no client could
    // key the slice it named (#4547). Gating restores that while keeping the
    // external-ownership contract #1807/#4407 added.
    const bool routeApplies
        = m_splitRequested || (m_rxSliceId == rxSliceId && m_txSliceId >= 0);

    // A TX slice another client operates as its receiver is never this
    // client's PTT target, whatever the cache says (#5193). The cache can be
    // stale in exactly this way: the route was bound while the slice was
    // unclaimed, and a client has declared it since. Nothing in the
    // audio_start / audio_stop handlers touches routing state, so the check
    // has to live here, at the point of trust. Callers without requester
    // knowledge pass no flags and see the pre-#5193 behaviour unchanged.
    const bool liveTxIsAnotherReceiver = currentTx >= 0 && currentTx != rxSliceId
        && operatedByAnotherClient(endpoints, currentTx);
    if (liveTxIsAnotherReceiver && m_owner == TxRouteOwner::External) {
        // Same rule as resolveVfoB()'s EchoOnly branch: an external bind is
        // only ever the live TX slice, so with that slice foreign there is
        // none to keep. A TciCreated one is left in place and refused below.
        m_rxSliceId = -1;
        m_txSliceId = -1;
        m_owner = TxRouteOwner::None;
    }

    if (routeApplies) {
        if (currentTx >= 0 && !liveTxIsAnotherReceiver) {
            // The live TX slice outranks the cache: m_txSliceId is only refreshed here,
            // in resolveVfoB and bindCreatedRoute, so a stale route would key the old
            // slice after the operator moved TX (#4547). Ownership must drop to External
            // on refresh: teardown issues `slice remove` for TciCreated routes, which would
            // delete an operator's slice.
            m_rxSliceId = rxSliceId;
            if (currentTx != m_txSliceId) {
                m_txSliceId = currentTx;
                m_owner = TxRouteOwner::External;
            }
            return currentTx;
        }
        // Backends that mark no TX slice at all (the seam backends; the Flex
        // always-one-TX-slice invariant does not hold there) still answer from
        // the tracked route. So does a negotiated route whose slice is not the
        // live TX slice because another client's receiver holds TX: the
        // tracked slice is promoted, the other client's is never keyed.
        if (m_txSliceId >= 0 && contains(endpoints, m_txSliceId)
            && !operatedByAnotherClient(endpoints, m_txSliceId)) {
            return m_txSliceId;
        }
    }

    // No route applies (or live TX is another client's receiver): key the named
    // slice; the caller promotes it. Route state is left alone: recording
    // rxSliceId would make a route bound for another RX slice apply on the next
    // PTT, and clearing it would orphan a TciCreated slice.
    return rxSliceId;
}

bool TciRoutingState::setSplitRequested(bool enabled)
{
    const bool changed = m_splitRequested != enabled;
    m_splitRequested = enabled;
    return changed;
}

void TciRoutingState::bindCreatedRoute(int rxSliceId, int txSliceId)
{
    m_rxSliceId = rxSliceId;
    m_txSliceId = txSliceId;
    m_owner = TxRouteOwner::TciCreated;
}

void TciRoutingState::clearTciRoute()
{
    if (!ownsRoute()) {
        return;
    }
    m_rxSliceId = -1;
    m_txSliceId = -1;
    m_owner = TxRouteOwner::None;
}

void TciRoutingState::removeSlice(int sliceId)
{
    if (sliceId == m_rxSliceId || sliceId == m_txSliceId) {
        m_rxSliceId = -1;
        m_txSliceId = -1;
        m_owner = TxRouteOwner::None;
        m_splitRequested = false;
    }
}

void TciRoutingState::reset()
{
    m_splitRequested = false;
    m_rxSliceId = -1;
    m_txSliceId = -1;
    m_owner = TxRouteOwner::None;
}

} // namespace AetherSDR
