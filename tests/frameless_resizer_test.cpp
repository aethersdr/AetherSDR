// Regression coverage for FramelessResizer's two pure-logic helpers, added
// per review on PR #4829 (frameless move/resize under xcb, #4827): neither
// continueManualResize()'s clamping arithmetic nor ownsWindow()'s
// parent-chain matching had a test, and the XTest-driven proof in that PR
// only exercises one platform's one drag. clampManualResize() and
// windowOwnsChain() are the same code the private instance methods call
// (FramelessResizer.h), pulled out static so they're reachable here without
// a full widget/window hierarchy.

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
    testWindowOwnsChain();

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
