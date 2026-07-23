#pragma once

#include <QVector>

namespace AetherSDR
{

struct TciSliceEndpoint
{
    int sliceId { -1 };
    bool isTx { false };
};

// Shared TCI-to-Flex routing state. Wire TRX indexes are intentionally absent:
// callers translate them to stable Flex slice IDs before using this class.
class TciRoutingState
{
public:
    enum class TxRouteOwner
    {
        None,
        External,
        TciCreated,
    };

    enum class RouteAction
    {
        UseExisting,
        PromoteExisting,
        Create,
        Unavailable,
    };

    struct RouteDecision
    {
        RouteAction action { RouteAction::Unavailable };
        int txSliceId { -1 };
        TxRouteOwner owner { TxRouteOwner::None };
    };

    RouteDecision resolveVfoB(int rxSliceId, const QVector<TciSliceEndpoint>& endpoints);
    int resolvePttSlice(int rxSliceId, const QVector<TciSliceEndpoint>& endpoints);

    bool setSplitRequested(bool enabled);
    bool splitRequested() const
    {
        return m_splitRequested;
    }

    void bindCreatedRoute(int rxSliceId, int txSliceId);
    void clearTciRoute();
    void removeSlice(int sliceId);
    void reset();

    int rxSliceId() const
    {
        return m_rxSliceId;
    }
    int txSliceId() const
    {
        return m_txSliceId;
    }
    TxRouteOwner owner() const
    {
        return m_owner;
    }
    bool ownsRoute() const
    {
        return m_owner == TxRouteOwner::TciCreated;
    }

private:
    static bool contains(const QVector<TciSliceEndpoint>& endpoints, int sliceId);
    static int currentTxSlice(const QVector<TciSliceEndpoint>& endpoints);

    bool m_splitRequested { false };
    int m_rxSliceId { -1 };
    int m_txSliceId { -1 };
    TxRouteOwner m_owner { TxRouteOwner::None };
};

} // namespace AetherSDR
