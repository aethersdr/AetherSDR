// #4567 regression: TCI receiver numbers must survive a mid-session slice
// destroy/recreate. The captured failure: [A(id0)=trx0, B(id1)=trx1], band
// switch destroys+recreates A, positional numbering renumbered B to receiver
// 0 under a live client. With TciTrxMap, the recreated slice (same Flex id)
// reclaims its binding and surviving slices never move.
//
// Pure-logic test (no RadioModel): binding acquisition, recreate reuse,
// lowest-free assignment, release, clear, and the trx_count floor. The
// map↔model interplay (positional fallback, live-slice resolution) is
// exercised on hardware — see PR verification.

//
// #5193 section (below the TciTrxMap cases): TciRoutingState::resolveVfoB()
// must never adopt a TX slice that another client operates as its receiver.
// Same pure-logic shape — endpoint vectors in, RouteDecision out.

#include "core/TciRoutingState.h"
#include "core/TciTrxMap.h"

#include <QVector>

#include <iostream>

using AetherSDR::TciRoutingState;
using AetherSDR::TciSliceEndpoint;
using AetherSDR::TciTrxMap;

namespace {

int failures = 0;

void expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    if (!condition)
        ++failures;
}

} // namespace

int main()
{
    // Creation order gets dense numbers from 0 — identical to the
    // positional policy for a session with no destroy/recreate.
    {
        TciTrxMap map;
        expect(map.acquire(0) == 0, "first slice binds trx 0");
        expect(map.acquire(1) == 1, "second slice binds trx 1");
        expect(map.acquire(0) == 0, "acquire is idempotent for a bound id");
        expect(map.trxCount(nullptr) == 2, "trx_count = highest bound + 1");
    }

    // The #4567 capture: recreate reclaims its number, survivor never moves.
    {
        TciTrxMap map;
        map.acquire(0);  // slice A
        map.acquire(1);  // slice B
        // Band switch: A removed (release deferred past the settle window —
        // the owner has NOT called release), then re-added with the same id.
        expect(map.acquire(0) == 0, "recreated slice reclaims its trx");
        expect(map.acquire(1) == 1, "surviving slice keeps its trx");
        expect(map.trxCount(nullptr) == 2, "count unchanged across recreate");
    }

    // A genuine close (deferred release fired) frees the number for reuse.
    {
        TciTrxMap map;
        map.acquire(0);
        map.acquire(1);
        map.release(0);
        expect(map.acquire(7) == 0, "freed trx is reused lowest-first");
        expect(map.acquire(1) == 1, "unrelated binding untouched by release");
        // A hole (release of trx 0 with trx 1 still bound) must not shrink
        // the advertised count below an index in use.
        map.release(7);
        expect(map.trxCount(nullptr) == 2,
               "count stays above a transient hole (trx 1 still bound)");
    }

    // Disconnect: everything resets; next session starts dense from 0.
    {
        TciTrxMap map;
        map.acquire(3);
        map.acquire(5);
        map.clear();
        expect(map.trxCount(nullptr) == 1, "cleared map advertises 1 (floor)");
        expect(map.acquire(5) == 0, "post-clear acquisition restarts at 0");
    }

    // Null-model lookups fall back safely (no map hit, no crash).
    {
        TciTrxMap map;
        expect(map.trxForSlice(nullptr, nullptr) == 0,
               "null slice falls back to positional default 0");
        expect(map.sliceForTrx(nullptr, 0) == nullptr,
               "null model resolves no slice");
        expect(map.txSliceTrxOrNone(nullptr) == -1,
               "null model has no TX slice (-1 sentinel)");
        expect(!map.trxHasLiveSlice(nullptr, 0),
               "null model carries no live slice");
    }

    // ---- #5193: VFO B never adopts another client's receiver -----------------
    using Action = TciRoutingState::RouteAction;
    using Owner = TciRoutingState::TxRouteOwner;

    // CONSTRUCTED from the measured topology (#5193 repro, FLEX-8400, 2026-09-13):
    // slice 0 = A, TX, operated by the RX1 WSJT-X instance; slice 1 = B, the
    // RX2 instance's receiver. The RX2 instance's `vfo:1,1,<hz>` used to tune
    // slice 0. Two-client topology => EchoOnly, and no route state is written.
    {
        TciRoutingState routing;
        QVector<TciSliceEndpoint> twoClients { { 0, true, true }, { 1, false, false } };
        const auto decision = routing.resolveVfoB(1, twoClients);
        expect(decision.action == Action::EchoOnly,
               "#5193: VFO B from the non-TX receiver echoes when the TX slice is another client's");
        expect(decision.txSliceId < 0, "#5193: echo decision names no TX slice");
        expect(routing.rxSliceId() < 0 && routing.txSliceId() < 0
                   && routing.owner() == Owner::None,
               "#5193: echo decision records no route (bare PTT stays bare)");
        // The bare PTT that follows keys the slice the client named (#4547 contract).
        expect(routing.resolvePttSlice(1, twoClients) == 1,
               "#5193: bare PTT after an echo still keys the requested slice");
    }

    // Stale bind (#5681 review round 2): a route legitimately bound while the
    // TX slice was unclaimed must not survive that slice becoming another
    // client's receiver. CONSTRUCTED from the measured topology above, with
    // the RX1 instance's audio_start arriving AFTER the RX2 instance's first
    // band change (client start order is not fixed).
    {
        TciRoutingState routing;
        QVector<TciSliceEndpoint> unclaimed { { 0, true, false }, { 1, false, false } };
        QVector<TciSliceEndpoint> claimed { { 0, true, true }, { 1, false, false } };
        // Step 1: RX2's channel-1 frame adopts slice 0 (the #1807 branch).
        expect(routing.resolveVfoB(1, unclaimed).action == Action::UseExisting
                   && routing.txSliceId() == 0 && routing.owner() == Owner::External,
               "#5193 stale bind: VFO B binds the unclaimed TX slice first");
        // Step 2: RX1 declares slice 0. Step 3: RX2's next band change echoes
        // AND drops the now-stale bind.
        expect(routing.resolveVfoB(1, claimed).action == Action::EchoOnly,
               "#5193 stale bind: the next VFO B frame echoes once the slice is claimed");
        expect(routing.rxSliceId() < 0 && routing.txSliceId() < 0
                   && routing.owner() == Owner::None,
               "#5193 stale bind: EchoOnly drops the stale external bind");
        // Step 4: RX2's bare PTT keys its own slice, not RX1's.
        expect(routing.resolvePttSlice(1, claimed) == 1,
               "#5193 stale bind: bare PTT after the echo keys the requested slice");
    }

    // Same stale bind, no band change in between: the declaration lands and
    // the client keys straight away. The PTT resolver itself must refuse the
    // cached slice (the EchoOnly drop above never ran).
    {
        TciRoutingState routing;
        QVector<TciSliceEndpoint> unclaimed { { 0, true, false }, { 1, false, false } };
        QVector<TciSliceEndpoint> claimed { { 0, true, true }, { 1, false, false } };
        routing.resolveVfoB(1, unclaimed);
        expect(routing.txSliceId() == 0, "#5193 stale bind (PTT first): route bound");
        expect(routing.resolvePttSlice(1, claimed) == 1,
               "#5193 stale bind (PTT first): PTT refuses the cached slice once another client operates it");
        expect(routing.txSliceId() < 0 && routing.owner() == Owner::None,
               "#5193 stale bind (PTT first): the stale external bind is dropped");
        // Unclaimed again (that client sent audio_stop): the #1807 adoption
        // returns on the next frame, nothing is permanently poisoned.
        expect(routing.resolveVfoB(1, unclaimed).action == Action::UseExisting,
               "#5193 stale bind: an unclaimed slice is adopted again after the drop");
    }

    // An external bind outliving a GUI TX move: bound onto slice 2 while it
    // held TX, TX moved to slice 0 by the operator, slice 0 then declared by
    // another client. Neither slice is this client's PTT target (live TX
    // beats the cache; the live TX slice is foreign) — PTT keys the
    // requested slice and the stale bind is dropped.
    {
        TciRoutingState routing;
        QVector<TciSliceEndpoint> txOnTwo {
            { 0, false, false }, { 1, false, false }, { 2, true, false } };
        QVector<TciSliceEndpoint> txMovedAndClaimed {
            { 0, true, true }, { 1, false, false }, { 2, false, false } };
        expect(routing.resolveVfoB(1, txOnTwo).action == Action::UseExisting
                   && routing.txSliceId() == 2,
               "#5193 moved TX: VFO B binds slice 2 while it holds TX");
        expect(routing.resolvePttSlice(1, txMovedAndClaimed) == 1,
               "#5193 moved TX: PTT keys the requested slice, not the slice TX left");
        expect(routing.txSliceId() < 0 && routing.owner() == Owner::None,
               "#5193 moved TX: the external bind onto the slice TX left is dropped");
    }

    // A TciCreated route is not dropped here but is refused as a PTT target while
    // another client operates it; PTT then keys the requested slice.
    {
        TciRoutingState routing;
        routing.bindCreatedRoute(1, 2);
        QVector<TciSliceEndpoint> createdClaimed {
            { 0, false, false }, { 1, false, false }, { 2, true, true } };
        expect(routing.resolvePttSlice(1, createdClaimed) == 1,
               "#5193: a TciCreated slice another client operates is not keyed");
        expect(routing.ownsRoute() && routing.txSliceId() == 2,
               "#5193: the TciCreated route is left in place by the PTT resolver");
    }

    // Requested split with a negotiated (TciCreated) slice that is not the
    // live TX slice because another client's receiver holds TX: the
    // negotiated slice is promoted, the other client's is never keyed.
    {
        TciRoutingState routing;
        routing.setSplitRequested(true);
        routing.bindCreatedRoute(1, 2);
        QVector<TciSliceEndpoint> foreignTx {
            { 0, true, true }, { 1, false, false }, { 2, false, false } };
        expect(routing.resolvePttSlice(1, foreignTx) == 2,
               "#5193: split PTT promotes the negotiated slice, never the other client's TX slice");
    }

    // Direction follows the TX flag, not the slice number (measured: TX moved
    // to slice 1, the RX1 instance's channel-1 frame retuned slice 1).
    {
        TciRoutingState routing;
        QVector<TciSliceEndpoint> txOnB { { 0, false, false }, { 1, true, true } };
        expect(routing.resolveVfoB(0, txOnB).action == Action::EchoOnly,
               "#5193: symmetric case - TX flag on the other client's slice also echoes");
    }

    // #1807 satellite contract, unchanged: one client, TX parked on a slice no
    // client operates -> still adopted as VFO B (the assertion the retired
    // tci_protocol_test carried, verbatim topology).
    {
        TciRoutingState routing;
        QVector<TciSliceEndpoint> satellite { { 4, false }, { 7, true } };
        const auto decision = routing.resolveVfoB(4, satellite);
        expect(decision.action == Action::UseExisting && decision.txSliceId == 7
                   && decision.owner == Owner::External,
               "#1807: an unoperated external TX slice is still adopted as VFO B");
        expect(routing.resolvePttSlice(4, satellite) == 7,
               "#1807: PTT then resolves to the adopted external TX slice");
    }

    // A requested split still negotiates a route, but never onto another
    // client's receiver: with the only TX slice claimed the decision falls
    // through to Create (the server then applies its capacity rule), never
    // UseExisting/PromoteExisting naming the claimed slice.
    {
        TciRoutingState routing;
        QVector<TciSliceEndpoint> twoClients { { 0, true, true }, { 1, false, false } };
        routing.setSplitRequested(true);
        const auto decision = routing.resolveVfoB(1, twoClients);
        expect(decision.action == Action::Create && decision.txSliceId < 0,
               "#5193: split with a claimed TX slice requests a distinct slice, never the claimed one");
        // A stale cached route pointing at the claimed slice is not promoted either.
        TciRoutingState cached;
        cached.bindCreatedRoute(1, 0);
        cached.setSplitRequested(true);
        const auto promoted = cached.resolveVfoB(1, twoClients);
        expect(promoted.action != Action::PromoteExisting && promoted.action != Action::UseExisting,
               "#5193: a cached route onto a claimed slice is not promoted");
    }

    // The requester's OWN slice holds TX and a second client shares that
    // receiver: the flag must not turn the single-slice "request a distinct
    // TX slice" contract into an echo.
    {
        TciRoutingState routing;
        QVector<TciSliceEndpoint> sharedOwnTx { { 0, true, true }, { 1, false, false } };
        const auto decision = routing.resolveVfoB(0, sharedOwnTx);
        expect(decision.action == Action::Create,
               "#5193: a shared receiver on the requester's own TX slice still requests a distinct slice");
    }

    // Flag ignored where it must be: an endpoint list without requester
    // knowledge (all flags false) behaves exactly as before.
    {
        TciRoutingState routing;
        QVector<TciSliceEndpoint> unflagged { { 0, true }, { 1, false } };
        expect(routing.resolveVfoB(1, unflagged).action == Action::UseExisting,
               "unflagged endpoints keep the pre-#5193 adoption (caller without requester)");
    }

    std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures
              << " failures)\n";
    return failures ? 1 : 0;
}
