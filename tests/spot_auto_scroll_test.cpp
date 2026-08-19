// decideSpotAutoScrollTarget() unit tests (#4889 review).
//
// SpotHub's flushSpotBatch() used to unconditionally follow the BOTTOM of
// the spot table view — a lambda copied from the append-only console panes,
// where that's correct. The spot model is prepend-based (newest spot is
// always source row 0), so that assumption is backwards whenever
// newest-first is on screen (unsorted, or Time descending), and it's
// self-latching: on an empty table value==maximum==0, so "follow" starts
// true and scrollToBottom() keeps it true forever.
//
// This four-branch sort-state decision is exercised here as a pure function,
// pulled out of DxClusterDialog specifically so it doesn't need that
// dialog's dependency graph (DxClusterClient, WsjtxClient,
// SpotCollectorClient, PotaClient, EibiClient, N1MMSpotClient, FreeDvClient,
// RadioModel, DxccColorProvider) just to construct.

#include "gui/SpotAutoScroll.h"

#include <QCoreApplication>

#include <cstdio>

namespace AetherSDR {

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// Column 0 is Time in every call site (SpotTableModel::ColTime); a
// different value is used here to prove the function doesn't hardcode it.
constexpr int kTimeColumn = 0;
constexpr int kOtherColumn = 2;  // e.g. DX Call

// Unsorted (sortColumn < 0) is defensive rather than reachable in the live
// app — QTableView::setSortingEnabled(true) applies a default sort
// indicator immediately — but the function's own contract doesn't depend
// on that, so it's pinned here directly.
static void testUnsortedFollowsTop()
{
    check(decideSpotAutoScrollTarget(-1, Qt::AscendingOrder, kTimeColumn, 0, 500)
              == SpotAutoScrollTarget::Top,
          "unsorted at the top follows to the top");
    check(decideSpotAutoScrollTarget(-1, Qt::AscendingOrder, kTimeColumn, 50, 500)
              == SpotAutoScrollTarget::None,
          "unsorted, scrolled away from the top, does not follow");
}

// Time descending is the state #4889 reports: newest spots are prepended
// to the top of the source order, and the empty-table self-latch
// (value==maximum==0) must not be mistaken for "at the bottom".
static void testTimeDescendingFollowsTop()
{
    check(decideSpotAutoScrollTarget(kTimeColumn, Qt::DescendingOrder, kTimeColumn, 0, 0)
              == SpotAutoScrollTarget::Top,
          "an empty table (value==maximum==0) follows to the top, not the bottom");
    check(decideSpotAutoScrollTarget(kTimeColumn, Qt::DescendingOrder, kTimeColumn, 1, 800)
              == SpotAutoScrollTarget::Top,
          "at the top (within the 2px tolerance) follows to the top");
    check(decideSpotAutoScrollTarget(kTimeColumn, Qt::DescendingOrder, kTimeColumn, 400, 800)
              == SpotAutoScrollTarget::None,
          "scrolled into the middle does not follow — this is the exact bug: "
          "the old bottom-follow logic dragged the viewport away from here");
    check(decideSpotAutoScrollTarget(kTimeColumn, Qt::DescendingOrder, kTimeColumn, 800, 800)
              == SpotAutoScrollTarget::None,
          "at the bottom (wrong end for this sort) does not follow");
}

// Time ascending puts the newest spot at the bottom — the one direction the
// original bottom-follow logic got right by accident.
static void testTimeAscendingFollowsBottom()
{
    check(decideSpotAutoScrollTarget(kTimeColumn, Qt::AscendingOrder, kTimeColumn, 800, 800)
              == SpotAutoScrollTarget::Bottom,
          "at the bottom follows to the bottom");
    check(decideSpotAutoScrollTarget(kTimeColumn, Qt::AscendingOrder, kTimeColumn, 799, 800)
              == SpotAutoScrollTarget::Bottom,
          "within the 2px tolerance of the bottom still follows");
    check(decideSpotAutoScrollTarget(kTimeColumn, Qt::AscendingOrder, kTimeColumn, 0, 800)
              == SpotAutoScrollTarget::None,
          "at the top (wrong end for this sort) does not follow");
    check(decideSpotAutoScrollTarget(kTimeColumn, Qt::AscendingOrder, kTimeColumn, 400, 800)
              == SpotAutoScrollTarget::None,
          "scrolled into the middle does not follow");
}

// Sorted by a non-Time column: the newest spot can land anywhere, so the
// only correct answer is "leave the viewport where the user put it" —
// never auto-scroll, regardless of scroll position.
static void testOtherColumnNeverFollows()
{
    check(decideSpotAutoScrollTarget(kOtherColumn, Qt::AscendingOrder, kTimeColumn, 0, 800)
              == SpotAutoScrollTarget::None,
          "sorted by another column, at the top, does not follow");
    check(decideSpotAutoScrollTarget(kOtherColumn, Qt::DescendingOrder, kTimeColumn, 800, 800)
              == SpotAutoScrollTarget::None,
          "sorted by another column, at the bottom, does not follow");
    check(decideSpotAutoScrollTarget(kOtherColumn, Qt::AscendingOrder, kTimeColumn, 0, 0)
              == SpotAutoScrollTarget::None,
          "sorted by another column on an empty table does not follow either — "
          "the self-latch trap must not resurface through this branch");
}

}  // namespace AetherSDR

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    AetherSDR::testUnsortedFollowsTop();
    AetherSDR::testTimeDescendingFollowsTop();
    AetherSDR::testTimeAscendingFollowsBottom();
    AetherSDR::testOtherColumnNeverFollows();

    if (AetherSDR::g_failures == 0)
        std::fprintf(stderr, "spot_auto_scroll_test: all checks passed\n");
    return AetherSDR::g_failures == 0 ? 0 : 1;
}
