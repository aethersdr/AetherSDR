// Regression coverage for FramelessResizer's pure-logic helpers, added per
// review on PR #4829 (frameless move/resize under xcb, #4827) and grown
// across two further review rounds. None of continueManualResize()'s
// clamping arithmetic, ownsWindow()'s parent-chain matching, edgesAt()'s
// margin/topMoveReserve math, or the ungrabbed-Leave end-drag decision had a
// test, and the XTest-driven proof in that PR only exercises one platform's
// one drag. clampManualResize(), windowOwnsChain(), computeEdges(), and
// shouldEndOnUngrabbedLeave() are the same code the private instance methods
// call (FramelessResizer.h), pulled out static so they're reachable here
// without a full widget/window hierarchy.
//
// computeEdges() coverage exists specifically because of a real regression a
// review round found: MainWindow's FramelessResizer::install() call left
// topMoveReserve at its default 0, so the resize band lived inside the 32px
// title bar and shadowed the menu bar and min/max/close controls (#4886).
// The topMoveReserve cases below are the regression test for that class of
// bug — a future adopter that installs with a title bar but forgets to pass
// topMoveReserve won't be caught here (that's a call-site/integration
// concern, not this function's), but if topMoveReserve *is* passed, these
// cases pin down that it actually suppresses TopEdge correctly.

#include "gui/FramelessResizer.h"

#include <QApplication>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QWindow>

#include <cstdio>
#include <string>

using AetherSDR::FramelessResizer;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

std::string rectStr(const QRect& r)
{
    return "(" + std::to_string(r.left()) + "," + std::to_string(r.top()) + " "
        + std::to_string(r.width()) + "x" + std::to_string(r.height()) + ")";
}

// A window well within its floor/ceiling: every edge just follows the
// pointer delta, origin-pinned edges hold, moved edges follow exactly.
void testUnclamped()
{
    const QRect start(100, 100, 400, 300);         // right=499, bottom=399
    const QSize floor(100, 100);
    const QSize ceiling(2000, 2000);

    const QRect right = FramelessResizer::clampManualResize(
        start, QPoint(50, 0), Qt::RightEdge, floor, ceiling);
    report("RightEdge: left/top/height pinned, width grows by delta",
           right.left() == 100 && right.top() == 100 && right.height() == 300
               && right.width() == 450,
           rectStr(right));

    const QRect bottom = FramelessResizer::clampManualResize(
        start, QPoint(0, -20), Qt::BottomEdge, floor, ceiling);
    report("BottomEdge: origin pinned, height shrinks by delta",
           bottom.left() == 100 && bottom.top() == 100 && bottom.width() == 400
               && bottom.height() == 280,
           rectStr(bottom));

    // Left/Top move the *origin*; the opposite edge is what must stay pinned.
    const QRect left = FramelessResizer::clampManualResize(
        start, QPoint(30, 0), Qt::LeftEdge, floor, ceiling);
    report("LeftEdge: right edge pinned, left moves and width shrinks",
           left.right() == start.right() && left.left() == 130
               && left.width() == 370,
           rectStr(left));

    const QRect topLeft = FramelessResizer::clampManualResize(
        start, QPoint(10, 10), Qt::TopEdge | Qt::LeftEdge, floor, ceiling);
    report("Top+Left corner: right/bottom pinned, origin moves on both axes",
           topLeft.right() == start.right() && topLeft.bottom() == start.bottom()
               && topLeft.left() == 110 && topLeft.top() == 110,
           rectStr(topLeft));
}

// Dragging an edge past the floor must stop at the floor, not overshoot —
// this is the "resize back past minimumSizeHint()" case #4827's manual path
// exists for in the first place (Qt would otherwise silently grow the
// window back while the origin kept moving, per the comment in
// clampManualResize()).
void testFloorClamp()
{
    const QRect start(0, 0, 400, 300);
    const QSize floor(200, 150);
    const QSize ceiling(2000, 2000);

    // Drag RightEdge far enough left to demand width < floor.
    const QRect right = FramelessResizer::clampManualResize(
        start, QPoint(-300, 0), Qt::RightEdge, floor, ceiling);
    report("RightEdge floor clamp: width never drops below floor",
           right.width() == floor.width() && right.left() == 0,
           rectStr(right));

    // Drag LeftEdge far enough right to demand width < floor — the origin
    // must stop moving once the floor is hit, not keep tracking the pointer.
    const QRect left = FramelessResizer::clampManualResize(
        start, QPoint(300, 0), Qt::LeftEdge, floor, ceiling);
    report("LeftEdge floor clamp: width never drops below floor, right pinned",
           left.width() == floor.width() && left.right() == start.right(),
           rectStr(left));

    const QRect bottom = FramelessResizer::clampManualResize(
        start, QPoint(0, -250), Qt::BottomEdge, floor, ceiling);
    report("BottomEdge floor clamp: height never drops below floor",
           bottom.height() == floor.height(),
           rectStr(bottom));

    const QRect top = FramelessResizer::clampManualResize(
        start, QPoint(0, 250), Qt::TopEdge, floor, ceiling);
    report("TopEdge floor clamp: height never drops below floor, bottom pinned",
           top.height() == floor.height() && top.bottom() == start.bottom(),
           rectStr(top));
}

// The maximumWidth()/maximumHeight() ceiling, symmetric to the floor case.
void testCeilingClamp()
{
    const QRect start(0, 0, 400, 300);
    const QSize floor(100, 100);
    const QSize ceiling(500, 350);

    const QRect right = FramelessResizer::clampManualResize(
        start, QPoint(300, 0), Qt::RightEdge, floor, ceiling);
    report("RightEdge ceiling clamp: width never exceeds maximumWidth()",
           right.width() == ceiling.width(),
           rectStr(right));

    const QRect left = FramelessResizer::clampManualResize(
        start, QPoint(-300, 0), Qt::LeftEdge, floor, ceiling);
    report("LeftEdge ceiling clamp: width never exceeds maximumWidth(), right pinned",
           left.width() == ceiling.width() && left.right() == start.right(),
           rectStr(left));
}

// A no-op edge selection (e.g. a hover with no button down never reaches
// this function, but a defensive check costs nothing) must return the start
// geometry unchanged.
void testNoEdges()
{
    const QRect start(10, 20, 300, 200);
    const QRect result = FramelessResizer::clampManualResize(
        start, QPoint(50, 50), Qt::Edges{}, QSize(50, 50), QSize(1000, 1000));
    report("No edges selected: geometry passes through unchanged",
           result == start,
           rectStr(result));
}

// The margin/topMoveReserve math edgesAt() applies — added for the #4829
// review round that found MainWindow's install() call left topMoveReserve
// at its default 0, so the resize band lived *inside* the title bar and
// shadowed the menu bar and min/max/close controls (#4886). These cases
// exist to keep that mechanism honest: a nonzero topMoveReserve must
// suppress TopEdge for every point above it, margin-only or not.
void testComputeEdges()
{
    const QRect win(0, 0, 400, 300);   // window-local rect, as edgesAt() sees it

    // No topMoveReserve: every edge and corner detected within margin.
    report("computeEdges: left edge",
           FramelessResizer::computeEdges(win, QPoint(0, 150), 6, 0, false)
               == Qt::LeftEdge);
    report("computeEdges: right edge",
           FramelessResizer::computeEdges(win, QPoint(399, 150), 6, 0, false)
               == Qt::RightEdge);
    report("computeEdges: top edge",
           FramelessResizer::computeEdges(win, QPoint(200, 0), 6, 0, false)
               == Qt::TopEdge);
    report("computeEdges: bottom edge",
           FramelessResizer::computeEdges(win, QPoint(200, 299), 6, 0, false)
               == Qt::BottomEdge);
    report("computeEdges: top-left corner",
           FramelessResizer::computeEdges(win, QPoint(0, 0), 6, 0, false)
               == (Qt::TopEdge | Qt::LeftEdge));
    report("computeEdges: outside every margin — no edges",
           FramelessResizer::computeEdges(win, QPoint(200, 150), 6, 0, false)
               == Qt::Edges{});

    // The regression this exists to catch: topMoveReserve=32 (TitleBar's
    // real height) must suppress TopEdge for the whole reserved strip, even
    // though y=5 is well within the 6px margin on its own.
    report("computeEdges: y=5 inside a 32px topMoveReserve — TopEdge suppressed",
           FramelessResizer::computeEdges(win, QPoint(200, 5), 6, 32, false)
               == Qt::Edges{});
    // topMoveReserve suppresses *every* edge in the reserved strip, not just
    // TopEdge — edgesAt()'s early return exits before Left/Right/Bottom are
    // even checked, matching its own comment that the whole strip is the
    // window's own move handling, not a resize zone. A point at x=0 (which
    // would be LeftEdge on its own) still gets nothing at y=5 < reserve=32.
    report("computeEdges: reserve suppresses ALL edges in the strip, not just Top"
           " — x=0,y=5 (would be LeftEdge) is still nothing",
           FramelessResizer::computeEdges(win, QPoint(0, 5), 6, 32, false)
               == Qt::Edges{});
    report("computeEdges: topMoveReserve boundary — y==reserve is no longer"
           " suppressed, but it's also past the margin so still no edge",
           FramelessResizer::computeEdges(win, QPoint(200, 32), 6, 32, false)
               == Qt::Edges{});
    report("computeEdges: below topMoveReserve, within margin of a closer edge — LeftEdge",
           FramelessResizer::computeEdges(win, QPoint(2, 40), 6, 32, false)
               == Qt::LeftEdge);

    // Maximized/fullscreen: no edges anywhere, topMoveReserve or not.
    report("computeEdges: maximized suppresses every edge",
           FramelessResizer::computeEdges(win, QPoint(0, 0), 6, 0, true)
               == Qt::Edges{});

    // A point outside the window rect entirely (reachable since native
    // children deliver events in global coordinates) — no edges, not a
    // false-positive edge from unclamped comparisons.
    report("computeEdges: point outside the window rect — no edges",
           FramelessResizer::computeEdges(win, QPoint(-10, 150), 6, 0, false)
               == Qt::Edges{});
}

// The decision behind the #4829 review-round-3 fix: an active manual resize
// with no real grab must end itself on Leave, or it strands m_manualResizeActive
// true until (at best) a buttonless move eventually arrives.
void testShouldEndOnUngrabbedLeave()
{
    report("shouldEndOnUngrabbedLeave: active + ungrabbed -> end it",
           FramelessResizer::shouldEndOnUngrabbedLeave(true, false));
    report("shouldEndOnUngrabbedLeave: active + grabbed -> leave it running",
           !FramelessResizer::shouldEndOnUngrabbedLeave(true, true));
    report("shouldEndOnUngrabbedLeave: inactive + ungrabbed -> nothing to end",
           !FramelessResizer::shouldEndOnUngrabbedLeave(false, false));
    report("shouldEndOnUngrabbedLeave: inactive + grabbed -> nothing to end",
           !FramelessResizer::shouldEndOnUngrabbedLeave(false, true));
}

void testWindowOwnsChain()
{
    QWindow mine;
    QWindow child(&mine);
    QWindow grandchild(&child);
    QWindow unrelated;   // separate top-level, null parent() — e.g. a dialog

    report("windowOwnsChain: a window owns itself",
           FramelessResizer::windowOwnsChain(&mine, &mine));
    report("windowOwnsChain: true for a direct native child",
           FramelessResizer::windowOwnsChain(&child, &mine));
    report("windowOwnsChain: true for a transitive (grandchild) descendant",
           FramelessResizer::windowOwnsChain(&grandchild, &mine));
    report("windowOwnsChain: false for an unrelated top-level window",
           !FramelessResizer::windowOwnsChain(&unrelated, &mine));
    report("windowOwnsChain: false when win is null",
           !FramelessResizer::windowOwnsChain(nullptr, &mine));
    report("windowOwnsChain: false when mine is null",
           !FramelessResizer::windowOwnsChain(&child, nullptr));
    report("windowOwnsChain: doesn't walk the wrong direction (mine is not a descendant of child)",
           !FramelessResizer::windowOwnsChain(&mine, &child));
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    std::printf("FramelessResizer pure-logic test harness (PR #4829 review)\n\n");

    testUnclamped();
    testFloorClamp();
    testCeilingClamp();
    testNoEdges();
    testComputeEdges();
    testShouldEndOnUngrabbedLeave();
    testWindowOwnsChain();

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
