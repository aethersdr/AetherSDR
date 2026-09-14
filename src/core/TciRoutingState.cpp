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

    // A TX slice that another client is operating as ITS receiver is not this
    // receiver's VFO B. Two WSJT-X instances on two slices each send
    // `vfo:<trx>,1,<hz>` on every band change (Split = Rig sends it with split
    // still false); because a Flex always marks exactly one TX slice, the
    // adoption below used to fire for whichever instance was not on the TX
    // slice and retune the OTHER instance's slice — and the radio's transmit
    // frequency — to its band (#5193, measured 21/21 on a FLEX-8400). The
    // single-client cases keep adopting: a satellite operator with TX parked
    // on a second slice that no client operates (#1807) still tunes it as
    // VFO B, and a requested split still negotiates a route below.
    // Only a FOREIGN TX slice can be another client's receiver for this
    // purpose: when the requester's own slice holds TX, the request follows
    // the pre-existing Create/Promote path whatever other clients share that
    // receiver (a second client on the same slice must not suppress the
    // single-slice "request a distinct TX slice" contract).
    const bool currentTxIsAnotherReceiver = currentTx >= 0 && currentTx != rxSliceId
        && operatedByAnotherClient(endpoints, currentTx);
    if (currentTxIsAnotherReceiver && !m_splitRequested) {
        // Records no NEW route: this is a decision about one frame, not a
        // route change, and writing m_rxSliceId here would make
        // resolvePttSlice()'s routeApplies true for a route never bound.
        //
        // An external bind recorded EARLIER (by any requester) is another
        // matter. The UseExisting branch below adopted the live TX slice
        // while no client had declared it (the #1807 shape); an external
        // bind is only ever the live TX slice, so once that slice is another
        // client's receiver there is no valid external bind at all, whether
        // the cache names this slice or one TX has since moved away from.
        // Left in place it would let resolvePttSlice() hand the other
        // client's receiver, or a slice TX has left, to the next bare PTT.
        // Drop it. A TciCreated route is not dropped here: resolvePttSlice()
        // refuses such a slice on its own, and dropping it would not help
        // teardown either way (see the Create branch below).
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
            // The live TX slice always outranks the cache. m_txSliceId is
            // refreshed only here, in resolveVfoB and in bindCreatedRoute, and
            // clearTciRoute() no-ops for a route TCI does not own — so a route
            // bound while slice 0 held TX outlives the operator moving TX to
            // slice 1 from the GUI, and returning it keys slice 0's band and
            // antenna with no operator action (#4547 secondary).
            //
            // Dropping ownership to External on refresh is load-bearing, not
            // bookkeeping: handleSplitRequest() issues `slice remove` for a
            // TciCreated route on teardown, so carrying that owner onto a
            // slice TCI never created would delete an operator's slice.
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

    // No route applies (or the live TX slice is another client's receiver):
    // key the slice the client named. The caller promotes it.
    //
    // Deliberately does NOT record rxSliceId. Writing it while m_txSliceId
    // still held a route bound for a *different* RX slice would make
    // routeApplies true on the next call for this slice, so the second bare
    // PTT in a row would silently start honouring a route that was never bound
    // for it. Clearing the pair instead is worse: it would orphan a TciCreated
    // slice that only handleSplitRequest()'s teardown knows how to remove. A
    // bare PTT is a decision, not a route change, so it leaves route state alone.
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
