// Regression coverage for WHEN radio-driven active-slice selection is treated
// as synchronization-only after a band recall. The decision itself is covered
// by band_recall_slice_selection_policy_test; this pins the window, whose two
// failure modes both put the waterfall back on the old band or suppress a
// selection that was never racing anything:
//   * opening it for a band write that never reached the wire, and
//   * closing it while the radio is still rebuilding the pan.

#include "gui/BandRecallSelectionGuard.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

constexpr int kWindowMs = 1500;
constexpr int kMaxWindowMs = 4000;

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

BandRecallSelectionGuard makeGuard()
{
    return BandRecallSelectionGuard(kWindowMs, kMaxWindowMs);
}

}  // namespace

int main()
{
    const QString pan = QStringLiteral("0x40000000");
    const QString otherPan = QStringLiteral("0x40000001");

    // Baseline: the window opens on dispatch and closes on its own.
    {
        BandRecallSelectionGuard guard = makeGuard();
        check(!guard.isActive(pan, 0), "idle: no window before a band write");
        guard.arm(pan, 1000);
        check(guard.isActive(pan, 1000), "armed: window is open at dispatch");
        check(guard.isActive(pan, 2499), "armed: window covers the grace period");
        check(!guard.isActive(pan, 2500), "armed: window closes at the grace edge");
        check(!guard.isActive(otherPan, 1000),
              "armed: window is per-pan, not global");
    }

    // The reported gap #1: `display pan set ... band=` is emitted before
    // sendCommand(), so a dropped write (foreign-owned pan, dead WAN session)
    // must not leave selection suppressed for a rebuild that never starts.
    {
        BandRecallSelectionGuard guard = makeGuard();
        guard.arm(pan, 1000);
        guard.cancelArm(pan);
        check(!guard.isActive(pan, 1000),
              "dispatch failed: window does not open for an undispatched write");
    }

    // ...but a failed write must not tear down a still-live window that an
    // earlier, successful recall on the same pan is still owed.
    {
        BandRecallSelectionGuard guard = makeGuard();
        guard.arm(pan, 1000);      // succeeded
        guard.arm(pan, 1200);      // superseding request, about to fail
        guard.cancelArm(pan);
        check(guard.isActive(pan, 1200),
              "dispatch failed: earlier live window survives the rollback");
        check(guard.isActive(pan, 2499),
              "dispatch failed: rollback restores the earlier deadline exactly");
        check(!guard.isActive(pan, 2500),
              "dispatch failed: rollback does not extend the earlier deadline");
    }

    // A rollback must only ever undo its own arm.
    {
        BandRecallSelectionGuard guard = makeGuard();
        guard.arm(pan, 1000);
        guard.cancelArm(otherPan);
        check(guard.isActive(pan, 1000),
              "dispatch failed: a different pan's rollback is a no-op");
        guard.cancelArm(pan);
        guard.cancelArm(pan);
        check(!guard.isActive(pan, 1000),
              "dispatch failed: a repeated rollback stays idempotent");
    }

    // The reported gap #2: over SmartLink/WAN the rebuild can outrun a fixed
    // window, and one late old-band active=1 after expiry recentres the pan.
    {
        BandRecallSelectionGuard guard = makeGuard();
        guard.arm(pan, 1000);
        guard.refresh(pan, 2400);   // a slice was recreated late in the rebuild
        check(guard.isActive(pan, 2600),
              "slow rebuild: reconstruction evidence extends the window");
        check(!guard.isActive(pan, 3900),
              "slow rebuild: the extension is still bounded by the grace period");
    }

    // The extension is capped, so a chatty session cannot hold selection
    // suppressed indefinitely.
    {
        BandRecallSelectionGuard guard = makeGuard();
        guard.arm(pan, 1000);
        for (qint64 nowMs = 1100; nowMs <= 4900; nowMs += 100) {
            guard.refresh(pan, nowMs);
        }
        check(guard.isActive(pan, 4999),
              "cap: the window does stay open right up to the hard cap");
        check(!guard.isActive(pan, 5000),
              "cap: continuous evidence cannot extend past the hard cap");
    }

    // A refresh extends a live window; it never reopens a closed one.
    {
        BandRecallSelectionGuard guard = makeGuard();
        guard.arm(pan, 1000);
        guard.refresh(pan, 5000);
        check(!guard.isActive(pan, 5000),
              "expired: a late refresh cannot reopen a closed window");
        guard.refreshAll(5100);
        check(!guard.isActive(pan, 5100),
              "expired: refreshAll cannot reopen a closed window either");
    }

    // A live slice removal arrives without a pan id, so it refreshes every live
    // window — and must leave pans with no window alone.
    {
        BandRecallSelectionGuard guard = makeGuard();
        guard.arm(pan, 1000);
        guard.refreshAll(2400);
        check(guard.isActive(pan, 2600),
              "slice removed: refreshAll extends the live window");
        check(!guard.isActive(otherPan, 2600),
              "slice removed: refreshAll opens no window on an untouched pan");
    }

    // nowMs is wall-clock, so an NTP step backwards mid-rebuild must not close
    // a window early — that is the same late-activation failure by another name.
    {
        BandRecallSelectionGuard guard = makeGuard();
        guard.arm(pan, 4000);
        guard.refresh(pan, 3000);
        check(guard.isActive(pan, 5400),
              "clock step: a backwards clock cannot shorten a live window");
    }

    // An empty pan id is the "no pan" sentinel MainWindow can hand us; it must
    // never open a window that isActive("") would then report.
    {
        BandRecallSelectionGuard guard = makeGuard();
        guard.arm(QString(), 1000);
        check(!guard.isActive(QString(), 1000),
              "empty pan: arming a pan-less recall is a no-op");
    }

    if (failures == 0) {
        std::printf("\nAll band-recall selection-guard tests passed.\n");
        return 0;
    }
    std::printf("\n%d band-recall selection-guard test(s) failed.\n", failures);
    return 1;
}
