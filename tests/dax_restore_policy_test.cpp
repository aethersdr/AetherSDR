// Unit tests for DaxRestorePolicy — the window that confines the last-session
// per-slice DAX restore (#1221) to the initial post-connect enumeration, and
// the quit-time prune that lets a config poisoned by #4558 self-heal.
//
// The #4558 steal is reproduced here as a sequence test (connect → enumerate →
// band-stack recreate) so the regression is pinned by the ordering that
// actually failed on the rig, not just by the flag's truth table.

#include "gui/DaxRestorePolicy.h"

#include <QString>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

// Live removals emit AFTER the slice leaves the model; the post-reconnect
// stale prune emits an id that already names a live new-session slice.
constexpr bool kLive = true;
constexpr bool kPrune = false;

// ── The restore window ─────────────────────────────────────────────────────

void testClosedBeforeAnyConnect()
{
    DaxRestorePolicy p;
    report("closed before any connect", !p.restoreAllowed());
}

void testOpensOnConnect()
{
    DaxRestorePolicy p;
    p.onConnected();
    report("opens on connect", p.restoreAllowed());
}

void testInitialEnumerationIsAddsOnly()
{
    // The launch-time restore must be untouched: the initial enumeration adds
    // slices and removes none, so the window stays open across all of them.
    DaxRestorePolicy p;
    p.onConnected();
    const bool sliceA = p.restoreAllowed();
    const bool sliceB = p.restoreAllowed();
    report("initial enumeration restores every slice", sliceA && sliceB);
}

void testLiveRemovalClosesWindow()
{
    DaxRestorePolicy p;
    p.onConnected();
    const bool closedHere = p.onSliceRemoved(kLive);
    report("first live removal closes the window", closedHere && !p.restoreAllowed());
}

void testCloseIsReportedOnce()
{
    // The caller logs the transition, so a second live removal must not claim
    // to have closed an already-closed window.
    DaxRestorePolicy p;
    p.onConnected();
    p.onSliceRemoved(kLive);
    report("close transition reported once", !p.onSliceRemoved(kLive));
}

void testPruneRemovalDoesNotCloseWindow()
{
    // A reconnect prunes the previous session's staged slices. Disarming on
    // those would break the restore exactly when a reconnect needs it.
    DaxRestorePolicy p;
    p.onConnected();
    const bool closedHere = p.onSliceRemoved(kPrune);
    report("stale prune does not close the window",
           !closedHere && p.restoreAllowed());
}

void testPruneThenLiveStillCloses()
{
    DaxRestorePolicy p;
    p.onConnected();
    p.onSliceRemoved(kPrune);
    p.onSliceRemoved(kPrune);
    report("live removal after prunes still closes",
           p.onSliceRemoved(kLive) && !p.restoreAllowed());
}

void testSettleTimeoutClosesWindow()
{
    // The session that never removes a slice: without the timeout, a slice
    // added hours later would still inherit a last-session key.
    DaxRestorePolicy p;
    p.onConnected();
    p.onSettleTimeout(p.generation());
    report("settle timeout closes the window", !p.restoreAllowed());
}

void testStaleTimeoutCannotCloseNextConnectsWindow()
{
    // The generation guard: connect, disconnect before the timer fires,
    // reconnect — the first connect's pending timeout must be inert.
    DaxRestorePolicy p;
    p.onConnected();
    const int firstGen = p.generation();
    p.onDisconnected();
    p.onConnected();
    p.onSettleTimeout(firstGen);
    report("previous connect's timeout cannot close this window",
           p.restoreAllowed() && p.generation() != firstGen);
}

void testCurrentTimeoutStillClosesAfterReconnect()
{
    DaxRestorePolicy p;
    p.onConnected();
    p.onDisconnected();
    p.onConnected();
    p.onSettleTimeout(p.generation());
    report("current generation's timeout still closes", !p.restoreAllowed());
}

void testDisconnectClosesWindow()
{
    DaxRestorePolicy p;
    p.onConnected();
    p.onDisconnected();
    report("disconnect closes the window", !p.restoreAllowed());
}

void testReconnectReopensWindow()
{
    DaxRestorePolicy p;
    p.onConnected();
    p.onSliceRemoved(kLive);
    p.onDisconnected();
    p.onConnected();
    report("reconnect reopens the window for its enumeration",
           p.restoreAllowed());
}

// ── The #4558 regression, as the sequence that actually failed ─────────────

void test4558BandRecallRecreateIsNotRestored()
{
    // Rig capture: connect, two slices enumerate (both restored), then one
    // band button click destroys and recreates slice 0. The recreate re-enters
    // the list at the tail and would resolve DaxChannel_SliceB — the steal.
    DaxRestorePolicy p;
    p.onConnected();

    const bool enumeratedA = p.restoreAllowed();   // slice 0 added
    const bool enumeratedB = p.restoreAllowed();   // slice 1 added

    p.onSliceRemoved(kLive);                       // band switch destroys slice 0
    const bool recreated = p.restoreAllowed();     // ...and recreates it

    report("#4558: enumeration restores, band-recall recreate does not",
           enumeratedA && enumeratedB && !recreated);
}

void test4558MidSessionSliceAddIsNotRestored()
{
    // Opening a second panadapter minutes in: the new slice keeps whatever live
    // assignment it gets (TCI auto-assign, defaults) instead of inheriting a
    // position key from the previous session.
    DaxRestorePolicy p;
    p.onConnected();
    p.onSliceRemoved(kLive);
    report("#4558: mid-session slice add is not restored", !p.restoreAllowed());
}

// ── The quit-time prune ────────────────────────────────────────────────────

void testPruneRemovesTailBeyondLiveSlices()
{
    const QStringList keys = DaxRestorePolicy::staleKeysToPrune(true, 1);
    const bool spansTail = keys.size() == DaxRestorePolicy::kMaxSliceIndex
                        && keys.first() == QStringLiteral("DaxChannel_SliceB")
                        && keys.last() == QStringLiteral("DaxChannel_SliceZ");
    report("prune removes every key beyond the live slices",
           spansTail && !keys.contains(QStringLiteral("DaxChannel_SliceA")));
}

void test4558StaleKeySelfHeals()
{
    // The steal's precondition: DaxChannel_SliceB=2 written by an old two-slice
    // quit, which the old bounded writer could never clean. A single-slice quit
    // must now remove it.
    const QStringList keys = DaxRestorePolicy::staleKeysToPrune(true, 1);
    report("#4558: stale SliceB is pruned by a single-slice quit",
           keys.contains(QStringLiteral("DaxChannel_SliceB")));
}

void testPruneKeepsLiveKeysOnAFullHouse()
{
    const QStringList keys = DaxRestorePolicy::staleKeysToPrune(
        true, DaxRestorePolicy::kMaxSliceIndex + 1);
    report("prune removes nothing when every position is live", keys.isEmpty());
}

void testRadioLessQuitPrunesNothing()
{
    // #4572 review 1: never connected, failed connect, or disconnected first.
    // Erasing here loses the restore data on every radio-less launch.
    const bool emptyList = DaxRestorePolicy::staleKeysToPrune(false, 0).isEmpty();
    const bool withSlices = DaxRestorePolicy::staleKeysToPrune(false, 2).isEmpty();
    report("radio-less quit prunes nothing", emptyList && withSlices);
}

void testConnectedButUnenumeratedQuitPrunesNothing()
{
    // #4572 review 2: RadioModel::onConnected() clears m_slices BEFORE emitting
    // connectionStateChanged(true), so "connected with an empty list" covers
    // the whole pre-enumeration window — plus a rejected GUI registration
    // (#4563) and an all-foreign slice list, which stay empty indefinitely.
    // Erasing there wipes A..Z on a quit that proves nothing about the layout.
    report("connected-but-unenumerated quit prunes nothing",
           DaxRestorePolicy::staleKeysToPrune(true, 0).isEmpty());
}

void testKeyForIndexMatchesPersistedNames()
{
    // The reader in onSliceAdded and the writer in closeEvent must agree with
    // the key names already on disk from every previous release.
    report("key names match the persisted scheme",
           DaxRestorePolicy::keyForIndex(0) == QStringLiteral("DaxChannel_SliceA")
        && DaxRestorePolicy::keyForIndex(1) == QStringLiteral("DaxChannel_SliceB")
        && DaxRestorePolicy::keyForIndex(DaxRestorePolicy::kMaxSliceIndex)
               == QStringLiteral("DaxChannel_SliceZ"));
}

}  // namespace

int main()
{
    testClosedBeforeAnyConnect();
    testOpensOnConnect();
    testInitialEnumerationIsAddsOnly();
    testLiveRemovalClosesWindow();
    testCloseIsReportedOnce();
    testPruneRemovalDoesNotCloseWindow();
    testPruneThenLiveStillCloses();
    testSettleTimeoutClosesWindow();
    testStaleTimeoutCannotCloseNextConnectsWindow();
    testCurrentTimeoutStillClosesAfterReconnect();
    testDisconnectClosesWindow();
    testReconnectReopensWindow();

    test4558BandRecallRecreateIsNotRestored();
    test4558MidSessionSliceAddIsNotRestored();

    testPruneRemovesTailBeyondLiveSlices();
    test4558StaleKeySelfHeals();
    testPruneKeepsLiveKeysOnAFullHouse();
    testRadioLessQuitPrunesNothing();
    testConnectedButUnenumeratedQuitPrunesNothing();
    testKeyForIndexMatchesPersistedNames();

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
