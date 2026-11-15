// Unit tests for KiwiRebindTracker — the policy that retains a KiwiSDR RX
// replacement across the slice remove->re-add a FLEX band-stack recall performs
// (#4158). Covers the four identity hazards raised in review:
//   1. expected band-recall remove/re-add -> re-bind;
//   2. ordinary close followed by same-id creation -> NO transfer;
//   3. reconnect stale-prune collision (non-live removal) -> never defers;
//   4. repeated removals before an earlier timer expires -> generation-safe.

#include "gui/KiwiRebindTracker.h"

#include <QString>
#include <cstdio>

using namespace AetherSDR;
using RA = KiwiRebindTracker::RemoveAction;

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

const QString kPan  = QStringLiteral("0x40000000");
const QString kPanB = QStringLiteral("0x41000000");
const QString kKiwi = QStringLiteral("kb5ag.ddns.net");

}  // namespace

int main()
{
    // 1. Expected band-recall: mark the pan, remove the Kiwi slice (defers),
    //    same id re-appears on the same pan -> re-bind to the same profile.
    {
        KiwiRebindTracker t;
        t.noteBandRecall(kPan);
        const RA a = t.onSliceRemoved(0, /*live=*/true, kKiwi);
        report("band-recall: Kiwi live removal defers",
               a.kind == RA::Defer && t.hasPending(0));
        const QString rebind = t.onSliceAdded(0, kPan);
        report("band-recall: same id+pan re-binds the same profile",
               rebind == kKiwi);
        report("band-recall: pending + marker consumed on re-bind",
               !t.hasPending(0) && !t.bandRecallPending(kPan));
        report("band-recall: expiry after re-bind is a no-op",
               !t.onGraceExpired(0, a.generation));
    }

    // 2. Ordinary close + same-id create (no band recall): must NOT transfer the
    //    Kiwi to the unrelated new slice; the pending survives to be finalized.
    {
        KiwiRebindTracker t;
        const RA a = t.onSliceRemoved(0, /*live=*/true, kKiwi);
        report("plain close: Kiwi live removal defers", a.kind == RA::Defer);
        const QString rebind = t.onSliceAdded(0, kPan);   // no noteBandRecall()
        report("plain close: same-id create does NOT re-bind (no band recall)",
               rebind.isEmpty() && t.hasPending(0));
        report("plain close: grace expiry then finalizes the real teardown",
               t.onGraceExpired(0, a.generation) && !t.hasPending(0));
    }

    // 2b. A band recall on a DIFFERENT pan must not re-bind this slice.
    {
        KiwiRebindTracker t;
        t.noteBandRecall(kPanB);
        t.onSliceRemoved(0, /*live=*/true, kKiwi);
        report("cross-pan: band recall on another pan does not re-bind",
               t.onSliceAdded(0, kPan).isEmpty() && t.hasPending(0));
    }

    // 3. Reconnect stale-prune: sliceRemoved(oldId) whose id still resolves to a
    //    live slice (live=false). Must NOT defer and must NOT create pending, so
    //    it can never later clear the live slice's assignment.
    {
        KiwiRebindTracker t;
        const RA a = t.onSliceRemoved(0, /*live=*/false, kKiwi);
        report("stale prune: non-live removal tears down now, no defer",
               a.kind == RA::TeardownNow && !t.hasPending(0));
    }

    // 3b. A non-Kiwi live removal also tears down now (no pending).
    {
        KiwiRebindTracker t;
        const RA a = t.onSliceRemoved(0, /*live=*/true, QString());
        report("non-Kiwi live removal tears down now, no defer",
               a.kind == RA::TeardownNow && !t.hasPending(0));
    }

    // 4. Repeated removals before the first timer fires: the first (older)
    //    generation's expiry must be a no-op; only the newest finalizes.
    {
        KiwiRebindTracker t;
        const RA a1 = t.onSliceRemoved(0, /*live=*/true, kKiwi);   // gen 1
        const RA a2 = t.onSliceRemoved(0, /*live=*/true, kKiwi);   // gen 2 (supersedes)
        report("re-removal supersedes the pending generation",
               a2.generation != a1.generation && t.hasPending(0));
        report("older-generation expiry is a no-op",
               !t.onGraceExpired(0, a1.generation) && t.hasPending(0));
        report("newest-generation expiry finalizes",
               t.onGraceExpired(0, a2.generation) && !t.hasPending(0));
    }

    // 4b. Re-bind then re-remove within the window: the FIRST timer (its entry
    //     already consumed by the re-bind) must not finalize the new pending.
    {
        KiwiRebindTracker t;
        t.noteBandRecall(kPan);
        const RA a1 = t.onSliceRemoved(0, /*live=*/true, kKiwi);   // gen 1
        (void)t.onSliceAdded(0, kPan);                             // re-bind, clears pending
        const RA a2 = t.onSliceRemoved(0, /*live=*/true, kKiwi);   // gen 2 (new pending)
        report("rebind-then-reremove: stale gen-1 expiry does not finalize gen 2",
               !t.onGraceExpired(0, a1.generation) && t.hasPending(0));
        report("rebind-then-reremove: gen-2 expiry finalizes",
               t.onGraceExpired(0, a2.generation) && !t.hasPending(0));
    }

    if (g_failed == 0) {
        std::printf("\nAll %d kiwi-rebind-tracker tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d kiwi-rebind-tracker tests failed.\n", g_failed, g_total);
    return 1;
}
