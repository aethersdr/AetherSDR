// Unit tests for the generation-safe policy that restores Center Lock after a
// FLEX band recall rebuilds the slices on a panadapter.

#include "gui/CenterLockRebindTracker.h"

#include <QString>
#include <QVector>

#include <cstdio>

using namespace AetherSDR;

namespace {

using Kind = CenterLockRebindTracker::Resolution::Kind;
using SliceIdentity = CenterLockRebindTracker::SliceIdentity;

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

const QString kPan = QStringLiteral("0x40000000");
const QString kOtherPan = QStringLiteral("0x41000000");

SliceIdentity owned(const QString& panId, int sliceId, const QString& letter)
{
    return {panId, sliceId, letter, true};
}

SliceIdentity foreign(const QString& panId, int sliceId, const QString& letter)
{
    return {panId, sliceId, letter, false};
}

}  // namespace

int main()
{
    // The common one-slice workflow follows the sole recreated receiver even
    // when FLEX changes both of its transient identifiers.
    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 0, "A", 1);
        report("single: locked removal preserves the recall",
               generation > 0 && tracker.onSliceRemoved(kPan, 0));
        const auto result = tracker.resolveAfterGrace(
            kPan, generation, {owned(kPan, 3, "C")});
        report("single: sole recreated slice restores despite new identity",
               result.kind == Kind::Restore && result.sliceId == 3
                   && !tracker.hasPending(kPan));
    }

    // Unrelated removals cannot borrow another receiver's recall claim.
    {
        CenterLockRebindTracker tracker;
        tracker.noteBandRecall(kPan, 0, "A", 1);
        report("identity: another slice does not preserve intent",
               !tracker.onSliceRemoved(kPan, 1));
        report("identity: another pan does not preserve intent",
               !tracker.onSliceRemoved(kOtherPan, 0));
    }

    // Multi-slice restoration requires numeric id AND radio-provided letter.
    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 1, "B", 2);
        tracker.onSliceRemoved(kPan, 1);
        const auto result = tracker.resolveAfterGrace(
            kPan, generation,
            {owned(kPan, 0, "A"), owned(kPan, 1, "B")});
        report("multiple: exact id and letter restore the locked slice",
               result.kind == Kind::Restore && result.sliceId == 1);
    }

    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 1, "B", 2);
        tracker.onSliceRemoved(kPan, 1);
        const auto result = tracker.resolveAfterGrace(
            kPan, generation,
            {owned(kPan, 0, "B"), owned(kPan, 1, "A")});
        report("multiple: id-only and letter-only matches are ambiguous",
               result.kind == Kind::ReleaseAmbiguous && result.sliceId < 0);
    }

    // A changed topology is still safe when its sole candidate retains both
    // identifiers; otherwise Center Lock must not transfer to it.
    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 1, "B", 2);
        tracker.onSliceRemoved(kPan, 1);
        const auto result = tracker.resolveAfterGrace(
            kPan, generation, {owned(kPan, 1, "B")});
        report("topology: multiple-to-one exact identity restores",
               result.kind == Kind::Restore && result.sliceId == 1);
    }

    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 1, "B", 2);
        tracker.onSliceRemoved(kPan, 1);
        const auto result = tracker.resolveAfterGrace(
            kPan, generation, {owned(kPan, 1, "A")});
        report("topology: multiple-to-one mismatch releases safely",
               result.kind == Kind::ReleaseAmbiguous);
    }

    // Foreign and cross-pan slices never participate in the result. The one
    // owned receiver remains unambiguous even when another client has slices.
    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 0, "A", 1);
        tracker.onSliceRemoved(kPan, 0);
        const auto result = tracker.resolveAfterGrace(
            kPan, generation,
            {foreign(kPan, 1, "A"), owned(kOtherPan, 2, "A"),
             owned(kPan, 4, "D")});
        report("Multi-Flex: foreign and other-pan slices are ignored",
               result.kind == Kind::Restore && result.sliceId == 4);
    }

    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 0, "A", 1);
        tracker.onSliceRemoved(kPan, 0);
        const auto result = tracker.resolveAfterGrace(
            kPan, generation, {foreign(kPan, 0, "A")});
        report("Multi-Flex: a foreign lookalike is not a replacement",
               result.kind == Kind::ReleaseMissing);
    }

    // No recreated owned slice clears dormant intent instead of allowing a
    // future unrelated receiver to inherit it.
    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 0, "A", 1);
        tracker.onSliceRemoved(kPan, 0);
        const auto result = tracker.resolveAfterGrace(kPan, generation, {});
        report("missing replacement: release is explicit and one-shot",
               result.kind == Kind::ReleaseMissing
                   && !tracker.hasPending(kPan));
    }

    // A rejected/no-op band command produces no removal, so the still-live
    // Center Lock is left alone when the window ends.
    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 0, "A", 1);
        const auto result = tracker.resolveAfterGrace(
            kPan, generation, {owned(kPan, 0, "A")});
        report("no removal: live lock remains unchanged",
               result.kind == Kind::NoAction && !tracker.hasPending(kPan));
    }

    // A rapid second band selection carries the absent locked identity into a
    // new generation; the older timer cannot consume or release it.
    {
        CenterLockRebindTracker tracker;
        const quint64 first = tracker.noteBandRecall(kPan, 0, "A", 1);
        tracker.onSliceRemoved(kPan, 0);
        const quint64 second = tracker.noteBandRecall(kPan, -1, {}, 0);
        const auto stale = tracker.resolveAfterGrace(
            kPan, first, {owned(kPan, 2, "C")});
        report("rapid recall: stale generation cannot resolve the new claim",
               first != second && stale.kind == Kind::NoAction
                   && tracker.hasPending(kPan));
        const auto current = tracker.resolveAfterGrace(
            kPan, second, {owned(kPan, 2, "C")});
        report("rapid recall: newest generation restores the sole slice",
               current.kind == Kind::Restore && current.sliceId == 2);
    }

    // Explicit user intent always outranks an automatic rebind.
    {
        CenterLockRebindTracker tracker;
        const quint64 generation = tracker.noteBandRecall(kPan, 0, "A", 1);
        tracker.onSliceRemoved(kPan, 0);
        const bool canceledPendingRecall = tracker.cancel(kPan);
        const auto result = tracker.resolveAfterGrace(
            kPan, generation, {owned(kPan, 0, "A")});
        report("user action: cancellation reports superseded recall intent",
               canceledPendingRecall);
        report("user action: cancellation makes the timer inert",
               result.kind == Kind::NoAction && !tracker.hasPending(kPan)
                   && !tracker.cancel(kPan));
    }

    if (g_failed == 0) {
        std::printf("\nAll %d Center Lock rebind tracker tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d Center Lock rebind tracker tests failed.\n",
                g_failed, g_total);
    return 1;
}
