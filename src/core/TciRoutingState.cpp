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
    if (currentTx >= 0 && currentTx != rxSliceId) {
        // Track the current RX slice unconditionally (see resolveVfoB).
        m_rxSliceId = rxSliceId;
        if (currentTx != m_txSliceId) {
            m_txSliceId = currentTx;
            m_owner = TxRouteOwner::External;
        }
        return currentTx;
    }

    if (m_txSliceId >= 0 && contains(endpoints, m_txSliceId)) {
        return m_txSliceId;
    }
    return currentTx >= 0 ? currentTx : rxSliceId;
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
