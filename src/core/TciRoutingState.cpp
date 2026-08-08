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
    if (currentTx >= 0 && currentTx != rxSliceId) {
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

    if (m_txSliceId >= 0 && m_txSliceId != rxSliceId && contains(endpoints, m_txSliceId)) {
        m_rxSliceId = rxSliceId;
        return { RouteAction::PromoteExisting, m_txSliceId, m_owner };
    }

    // A non-TX slice may be an operator's independent receiver. Without an
    // explicit ownership signal, commandeering and retuning it is unsafe.
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

    if (routeApplies) {
        if (currentTx >= 0) {
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
        // the tracked route.
        if (m_txSliceId >= 0 && contains(endpoints, m_txSliceId)) {
            return m_txSliceId;
        }
    }

    // No route applies: key the slice the client named. The caller promotes it.
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
