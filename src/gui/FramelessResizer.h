#pragma once

#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <Qt>

class QWidget;
class QWindow;

namespace AetherSDR {

// Adds all-edge resize to a Qt::FramelessWindowHint top-level QWidget (#4827).
//
// Two platform quirks shape it:
//
//  1. Native child windows. Once any child acquires a native window Qt
//     promotes its siblings too, and pointer events go to the innermost native
//     window, so a filter on the top-level QWindow never sees them (MainWindow
//     on Linux/xcb: QStatusBar, the central widget and QSizeGrip are native).
//     The filter therefore sits on the application object and maps each event
//     window back to its top-level widget. Hover uses window-level events,
//     since button-less MouseMove only reaches widgets with mouse tracking.
//     On Windows MainWindow is not frameless (no-op); on macOS SpectrumWidget
//     sets WA_DontCreateNativeAncestors (#4339), so promotion does not happen.
//
//  2. startSystemResize() is unreliable on xcb and Windows+translucent
//     (QTBUG-69716 / QTBUG-90628, FramelessMoveHelper::
//     systemMoveResizeUnreliable()) and always fails on macOS (QCocoaWindow has
//     no override), so the press handler falls back to dragging the geometry
//     manually, as QSizeGrip does.
//
// Application-level filters run before the receiver's own event(), so a press
// inside the margin band never reaches a widget under it. MainWindow installs
// with topMoveReserve = TitleBar::kHeight to keep the title bar's menu and
// window buttons clickable; the bottom band still overlaps status-bar controls
// by ~4px (#4886).
//
// The automation bridge's pointer verbs send QMouseEvents to QWidgets, not
// QWindows, so they cannot exercise this filter; it was verified with real X11
// input (XTest).
//
// Known artifact of the manual path: dragging the left or top edge can make the
// opposite edge shimmer under a compositor. Geometry is exact; an app-driven
// setGeometry() has no _NET_WM_SYNC_REQUEST handshake, so stale content can be
// presented at the new origin.
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

    // The margin/reserve math edgesAt() applies: which edges (if any) a
    // window-local point `p` is within `margin` px of, given the window's
    // own `rect`. `topMoveReserve` excludes TopEdge for any point above that
    // many px regardless of margin — the mechanism MainWindow's
    // `topMoveReserve = TitleBar::kHeight` relies on to keep the resize band
    // out of the title bar (#4886/#4827 review round 3). No edges at all
    // while `maximizedOrFullscreen`, or for a point outside `rect` entirely
    // (reachable since events now arrive from native children carrying
    // global coordinates).
    static Qt::Edges computeEdges(const QRect& rect, const QPoint& p, int margin,
                                   int topMoveReserve, bool maximizedOrFullscreen);

    // The decision QEvent::Leave applies when a manual resize is active but
    // never got a real mouse grab: true means end the drag right there,
    // because without a grab we stop receiving any further events for it —
    // including the release — once the pointer leaves our window.
    static bool shouldEndOnUngrabbedLeave(bool manualResizeActive,
                                           bool manualResizeGrabbed);

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
