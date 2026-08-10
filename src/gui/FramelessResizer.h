#pragma once

#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <Qt>

class QWidget;
class QWindow;

namespace AetherSDR {

// Adds all-edge resize to a Qt::FramelessWindowHint top-level QWidget.
//
// Two platform quirks shape this class (both surfaced chasing #4827 — edge
// resize and title-bar move dead under `QT_QPA_PLATFORM=xcb`):
//
//  1. Native child windows.  The filter cannot live on the top-level QWindow.
//     Once any child widget acquires a native window, Qt promotes all of its
//     siblings to native too, and the platform then delivers pointer events to
//     the *innermost* native window — so the top-level QWindow never sees them.
//     MainWindow hits this: QStatusBar, the central QWidget, and QSizeGrip are
//     all native, between them covering the whole client area, which left every
//     edge dead while this same code worked fine on dialogs that happen to have
//     no native children.  The filter therefore sits on the application object
//     and matches events by resolving each event window back to its owning
//     top-level widget.  Hover detection uses window-level events rather than
//     widget-level ones because a button-less QEvent::MouseMove only reaches a
//     widget that opted into setMouseTracking().
//
//  2. xcb, and Windows+translucent.  QWindow::startSystemResize() reports
//     success but the WM-driven grab it hands off to is unreliable there
//     (QTBUG-69716 / QTBUG-90628 — see
//     FramelessMoveHelper::systemMoveResizeUnreliable()), so the resizer drags
//     the window geometry manually on those platforms, the same fallback Qt's
//     own QSizeGrip uses. macOS is a third case, but a distinct one:
//     QCocoaWindow has no startSystemResize() override at all and
//     unconditionally returns false, so the press handler below falls back to
//     the same manual drag whenever the call reports failure, rather than
//     needing its own platform check.
//
// Cross-platform behavior change (quirk 1 is not xcb-specific, but its actual
// reach is narrower than "everywhere" — checked per platform rather than
// asserted, see PR #4829): before this fix, MainWindow's edge-hover/press
// events were swallowed by its native children wherever that promotion
// happens, not only under xcb — the bug is about event routing through
// native child promotion, which xcb testing merely surfaced. Moving the
// filter application-wide turns on a real 6px edge-resize band on
// MainWindow, but only where both preconditions hold:
//   - MainWindow must actually be frameless. On Windows, MainWindow.cpp
//     never sets Qt::FramelessWindowHint at all (`#ifndef Q_OS_WIN` around
//     that call) — the FramelessWindowHint guard a few lines below then
//     makes this whole class a no-op there regardless of the filter's
//     reach, so quirk 1 has nothing to change on Windows.
//   - Native child promotion must actually be happening. On macOS,
//     SpectrumWidget deliberately blocks it: `applyNativeWindowIsolationPolicy()`
//     (SpectrumWidget.cpp) pairs `Qt::WA_NativeWindow` (needed for its Metal
//     QRhiWidget surface) with `Qt::WA_DontCreateNativeAncestors`
//     specifically so that native leaf never promotes its QWidget ancestors
//     (#4339) — the same cascade that leaves MainWindow's QStatusBar/central
//     widget/QSizeGrip native on Linux. So quirk 1 doesn't reach MainWindow
//     on macOS either. What exactly triggers the cascade on Linux/xcb
//     specifically (as opposed to macOS's deliberately-blocked case) wasn't
//     pinned down beyond the original `xwininfo -id <winid> -children`
//     evidence — the fix doesn't depend on knowing why, only on native
//     children provably swallowing events there.
// Net effect: this class's practical reach for MainWindow is Linux/X11
// (xcb) — real on that platform, a no-op on Windows, and a no-op for quirk 1
// specifically on macOS (macOS still gets quirk 2's fallback for the
// independent startSystemResize() problem above). Every other frameless
// adopter (dialogs, floating panels) was unaffected by quirk 1 to begin
// with, per the header's own note above that "this same code worked fine on
// dialogs that happen to have no native children."
//
// Edge-resize itself is proven end-to-end, not just eyeballed: a real
// XTestFakeButtonEvent/XTestFakeMotionEvent drag (genuine X11 input, not an
// app-level synthetic event — see PR #4829) on a running MainWindow's right
// edge under xcb grew it by exactly the dragged delta, independently
// confirmed by the resulting `display pan set … xpixels=` line the resize
// pushed to the radio. AetherSDR's own automation-bridge pointer verbs were
// tried first and don't work for this: `clickAt`/`dragAt`/`gesture`
// synthesize `QMouseEvent`s straight to the target *QWidget* via
// `QCoreApplication::sendEvent()`, never to its QWindow, so they never reach
// this filter, which by design (quirk 1 above) only acts on window-level
// events — the same reason native children swallowed the original bug. A
// bridge-driven proof would need a window-targeted synthetic event path
// added to the bridge; real OS-level input (as above) was the workaround.
//
// Shadowed-control check on MainWindow: confirmed live under xcb via a
// read-only widgetAt/childAt probe at the running window's bottom-edge margin
// — the status bar (46px, docked at the window's bottom edge) hosts several
// clickable controls whose rects genuinely extend into the bottom 6px:
// `gpsStatusButton` by ~4px, the "Cancel transmit" status label by ~1px. A
// press landing there is consumed by this class before the widget ever sees
// it — Qt calls application-installed event filters ahead of the receiver's
// own event() for every dispatch, so that is guaranteed once a press reaches
// this filter (proven above), not merely likely. Filed as a follow-up rather
// than fixed here (#4886): the fix belongs in the status bar
// layout (cap each stack's maximumHeight so its clickable area stops short
// of the edge) rather than in this class, which has no way to know about a
// specific adopter's child geometry.
//
// Known artifact of that manual path: dragging an edge that moves the window
// origin (left or top) can make the opposite edge appear to shimmer under a
// compositor.  The geometry itself is exact — the requested opposite edge is
// pinned by construction, and sampling the X window through a drag shows it
// never deviates.  What moves is the presentation: a WM-driven resize withholds
// the frame until the client acknowledges the new size (_NET_WM_SYNC_REQUEST),
// whereas an app-driven setGeometry() gets no such handshake, so the compositor
// can present content drawn for the previous size at the new origin.  Resizing
// from the right or bottom never shows it, because with the origin fixed the
// stale content is simply clipped.  Not reproducible via the platform path,
// which is exactly the path this quirk denies us.
//
// Usage:
//   FramelessResizer::install(this);       // from a QWidget constructor
//   FramelessResizer::install(win, 6);     // explicit margin
class FramelessResizer : public QObject {
    Q_OBJECT
public:
    // `topMoveReserve`: height (px) of an edge-to-edge move handle (e.g. a title
    // bar) at the top of the window. The top strip is reserved for the window's
    // own move handling instead of the top-edge resize zone, so a title-bar grab
    // isn't stolen by the resizer (#4266). 0 (default) keeps the full top edge
    // resizable — the behavior for adopters that inset their title bar.
    static void install(QWidget* window, int margin = 6, int topMoveReserve = 0);
    ~FramelessResizer() override;

    // Pure logic pulled out of the private instance methods below so it's
    // reachable from a unit test without constructing a real QWidget/QWindow
    // hierarchy (frameless_resizer_test.cpp). Behavior is identical either
    // way — continueManualResize()/ownsWindow() just forward into these.

    // The clamping arithmetic continueManualResize() applies each motion
    // event: anchors whichever edges aren't in `edges`, moves the rest by
    // `delta`, and holds every edge within [floor, ceiling]. `floor` is
    // already minimumSize().expandedTo(minimumSizeHint()) — see
    // m_manualResizeFloor — and `ceiling` is
    // {maximumWidth(), maximumHeight()}.
    static QRect clampManualResize(const QRect& startGeom, const QPoint& delta,
                                    Qt::Edges edges, const QSize& floor,
                                    const QSize& ceiling);

    // The parent-chain walk ownsWindow() applies: true when `win` is `mine`
    // itself, or is parented (directly or transitively) to `mine` in the
    // QWindow tree — the shape a native child's promoted window takes
    // (quirk 1). A separate top-level (e.g. a dialog) has a null parent()
    // and only a transientParent, so this never reaches across windows.
    static bool windowOwnsChain(const QWindow* win, const QWindow* mine);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    explicit FramelessResizer(QWidget* window, int margin, int topMoveReserve);
    Qt::Edges edgesAt(const QPoint& windowPos) const;
    void enterEdgeZone(Qt::Edges edges);
    void leaveEdgeZone();

    // True when `win` is our top-level widget's window, or the window of one of
    // its native descendants (quirk 1 above). Thin wrapper over windowOwnsChain().
    bool ownsWindow(QWindow* win) const;

    // Manual drag fallback (xcb — quirk 2 above).  Anchors whichever edges
    // weren't grabbed and resizes the rest by the pointer delta, clamped to the
    // window's own min/max size. Thin wrapper over clampManualResize().
    void beginManualResize(Qt::Edges edges, const QPoint& pressGlobal);
    void continueManualResize(const QPoint& globalPos);
    void endManualResize();

    QWidget*  m_window{nullptr};
    int       m_margin{6};
    int       m_topMoveReserve{0};
    bool      m_cursorOverridden{false};
    Qt::Edges m_lastEdges{};

    bool      m_manualResizeActive{false};
    // True only when setMouseGrabEnabled(true) actually succeeded in
    // beginManualResize(). Gates the eventFilter() ownsWindow() bypass below
    // it: without a real grab we have no exclusivity guarantee, so treating
    // "a resize is active" alone as license to accept events from *any*
    // window — including an unrelated dialog the user happens to be
    // clicking in at the same time — would let that unrelated activity
    // drive our resize.
    bool      m_manualResizeGrabbed{false};
    Qt::Edges m_manualResizeEdges{};
    QPoint    m_manualResizePressGlobal;
    QRect     m_manualResizeStartGeom;
    QRect     m_manualResizeLastRequested;
    // minimumSize().expandedTo(minimumSizeHint()), snapshotted once at press
    // time rather than recomputed on every motion event — minimumSizeHint()
    // walks the widget's layout, and a drag can produce dozens of moves.
    QSize     m_manualResizeFloor;
};

} // namespace AetherSDR
