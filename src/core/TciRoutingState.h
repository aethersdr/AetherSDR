#pragma once

#include <QVector>

namespace AetherSDR
{

struct TciSliceEndpoint
{
    int sliceId { -1 };
    bool isTx { false };
    // Another connected TCI client is operating this slice as its own receiver
    // (it declared `audio_start:<n>` on the trx bound to this slice). Such a
    // slice is never adopted as a different receiver's VFO B (#5193). Callers
    // that do not know the requester leave it false.
    bool operatedByAnotherClient { false };
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
        // This receiver has no VFO B of its own right now: the only TX slice
        // is another client's receiver and no split was requested. Answer the
        // request with the RX slice's frequency and touch nothing (#5193).
        EchoOnly,
        Unavailable,
    };

    struct RouteDecision
    {
        RouteAction action { RouteAction::Unavailable };
        int txSliceId { -1 };
        TxRouteOwner owner { TxRouteOwner::None };
    };

    // The slice that actually holds transmit right now, straight from the live
    // endpoint list; -1 when no slice is TX. Public so a caller comparing the
    // cached route against the live assignment uses this class's own notion of
    // "the live TX slice" rather than a second copy that can drift from it.
    static int currentTxSlice(const QVector<TciSliceEndpoint>& endpoints);

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
    static bool operatedByAnotherClient(
        const QVector<TciSliceEndpoint>& endpoints, int sliceId);

    bool m_splitRequested { false };
    int m_rxSliceId { -1 };
    int m_txSliceId { -1 };
    TxRouteOwner m_owner { TxRouteOwner::None };
};

} // namespace AetherSDR
