#pragma once

#include <QObject>
#include <QPoint>
#include <QRect>
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
//  2. xcb.  QWindow::startSystemResize() reports success but the WM-driven grab
//     it hands off to is unreliable there (QTBUG-69716 — see
//     FramelessMoveHelper::systemMoveResizeUnreliable()), so the resizer drags
//     the window geometry manually on that platform, the same fallback Qt's own
//     QSizeGrip uses.
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

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    explicit FramelessResizer(QWidget* window, int margin, int topMoveReserve);
    Qt::Edges edgesAt(const QPoint& windowPos) const;
    void enterEdgeZone(Qt::Edges edges);
    void leaveEdgeZone();

    // True when `win` is our top-level widget's window, or the window of one of
    // its native descendants (quirk 1 above).
    bool ownsWindow(QWindow* win) const;

    // Manual drag fallback (xcb — quirk 2 above).  Anchors whichever edges
    // weren't grabbed and resizes the rest by the pointer delta, clamped to the
    // window's own min/max size.
    void beginManualResize(Qt::Edges edges, const QPoint& pressGlobal);
    void continueManualResize(const QPoint& globalPos);
    void endManualResize();

    QWidget*  m_window{nullptr};
    int       m_margin{6};
    int       m_topMoveReserve{0};
    bool      m_cursorOverridden{false};
    Qt::Edges m_lastEdges{};

    bool      m_manualResizeActive{false};
    Qt::Edges m_manualResizeEdges{};
    QPoint    m_manualResizePressGlobal;
    QRect     m_manualResizeStartGeom;
    QRect     m_manualResizeLastRequested;
};

} // namespace AetherSDR
