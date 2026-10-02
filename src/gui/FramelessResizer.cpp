#include "FramelessResizer.h"
#include "FramelessMoveHelper.h"
#include "core/LogManager.h"

#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <algorithm>

namespace AetherSDR {

void FramelessResizer::install(QWidget* window, int margin, int topMoveReserve)
{
    new FramelessResizer(window, margin, topMoveReserve);
}

FramelessResizer::FramelessResizer(QWidget* window, int margin, int topMoveReserve)
    : QObject(window), m_window(window), m_margin(margin),
      m_topMoveReserve(topMoveReserve)
{
    // Application-wide: see quirk 1 in the header. Non-mouse events exit on a
    // type compare, but each frameless window installs its own instance and
    // PersistentDialog (no WA_DeleteOnClose) keeps instances alive after close,
    // so per-event cost scales with live instances, not call sites.
    if (QCoreApplication* app = QCoreApplication::instance()) {
        app->installEventFilter(this);
    }
}

FramelessResizer::~FramelessResizer()
{
    // Only global state is cleaned up here.  This deliberately does not touch
    // m_window: as a QObject child of that widget, this destructor runs during
    // the widget's own teardown, when its native window may already be gone.
    // An outstanding mouse grab dies with that window anyway; the override
    // cursor does not, and leaking it would strand a resize cursor on the
    // desktop or another window.
    m_manualResizeActive = false;
    leaveEdgeZone();
}

// static
bool FramelessResizer::windowOwnsChain(const QWindow* win, const QWindow* mine)
{
    if (!win || !mine) {
        return false;
    }
    // Walk the window parent chain: a native child widget's window is parented
    // to its nearest native ancestor's, so this catches every descendant.
    // Deliberately not QWidget::find(winId()), which depends on the widget's id
    // still being current in Qt's WId mapper — destroying and recreating a
    // native window can invalidate that, and code doing exactly that is a
    // standing workaround for platform window-state bugs.  The parent chain
    // carries no such dependency.  A separate top-level (a dialog) has a null
    // parent() and only a transientParent, so this never reaches across windows.
    for (const QWindow* w = win; w; w = w->parent()) {
        if (w == mine) {
            return true;
        }
    }
    return false;
}

bool FramelessResizer::ownsWindow(QWindow* win) const
{
    return windowOwnsChain(win, m_window->windowHandle());
}

// static
Qt::Edges FramelessResizer::computeEdges(const QRect& rect, const QPoint& p, int margin,
                                          int topMoveReserve, bool maximizedOrFullscreen)
{
    // A maximized or fullscreen window must not resize from an edge drag — the
    // compositor owns its geometry. Previously each adopter guarded this in its
    // own edgesAt(); centralizing it here closes the gap for every adopter
    // (#4266).
    if (maximizedOrFullscreen) {
        return {};
    }
    // Reserve an edge-to-edge top move handle (e.g. a full-width title bar) for
    // the window's own move handling rather than the top-edge resize zone, so a
    // title-bar grab isn't consumed as a resize (#4266). Resize still works
    // everywhere below the reserved strip.
    if (topMoveReserve > 0 && p.y() < topMoveReserve) {
        return {};
    }
    // Events now arrive from native children and carry global coordinates, so
    // a point outside the window is reachable in a way it wasn't before.
    if (!rect.contains(p)) {
        return {};
    }
    Qt::Edges edges;
    if (p.x() <= margin)                   edges |= Qt::LeftEdge;
    if (p.x() >= rect.width()  - margin)   edges |= Qt::RightEdge;
    if (p.y() <= margin)                   edges |= Qt::TopEdge;
    if (p.y() >= rect.height() - margin)   edges |= Qt::BottomEdge;
    return edges;
}

// static
bool FramelessResizer::shouldEndOnUngrabbedLeave(bool manualResizeActive,
                                                  bool manualResizeGrabbed)
{
    return manualResizeActive && !manualResizeGrabbed;
}

Qt::Edges FramelessResizer::edgesAt(const QPoint& p) const
{
    return computeEdges(m_window->rect(), p, m_margin, m_topMoveReserve,
                         m_window->isMaximized() || m_window->isFullScreen());
}

// Known limitation: setOverrideCursor()/restoreOverrideCursor() is one global
// LIFO shared by all instances. The deferred Leave pop and the cross-window pop
// in eventFilter() can pop after another instance pushed, restoring its entry.
// Balanced, so nothing leaks; the cursor shape is briefly wrong when moving
// straight from one frameless window's edge onto another's.
void FramelessResizer::enterEdgeZone(Qt::Edges edges)
{
    if (edges == m_lastEdges && m_cursorOverridden) return;

    Qt::CursorShape shape;
    if      (edges == (Qt::TopEdge    | Qt::LeftEdge))  shape = Qt::SizeFDiagCursor;
    else if (edges == (Qt::TopEdge    | Qt::RightEdge)) shape = Qt::SizeBDiagCursor;
    else if (edges == (Qt::BottomEdge | Qt::LeftEdge))  shape = Qt::SizeBDiagCursor;
    else if (edges == (Qt::BottomEdge | Qt::RightEdge)) shape = Qt::SizeFDiagCursor;
    else if (edges & (Qt::LeftEdge  | Qt::RightEdge))   shape = Qt::SizeHorCursor;
    else                                                 shape = Qt::SizeVerCursor;

    if (m_cursorOverridden) QGuiApplication::restoreOverrideCursor();
    QGuiApplication::setOverrideCursor(QCursor(shape));
    m_cursorOverridden = true;
    m_lastEdges = edges;
}

void FramelessResizer::leaveEdgeZone()
{
    if (m_cursorOverridden) {
        QGuiApplication::restoreOverrideCursor();
        m_cursorOverridden = false;
        m_lastEdges = {};
    }
}

void FramelessResizer::beginManualResize(Qt::Edges edges, const QPoint& pressGlobal)
{
    m_manualResizeActive = true;
    m_manualResizeEdges = edges;
    m_manualResizePressGlobal = pressGlobal;
    m_manualResizeStartGeom = m_window->geometry();
    m_manualResizeLastRequested = m_manualResizeStartGeom;
    // Snapshot once here rather than recomputing in continueManualResize() on
    // every motion event: minimumSizeHint() walks the widget's layout, and a
    // drag can produce dozens of moves before the release.
    m_manualResizeFloor = m_window->minimumSize().expandedTo(m_window->minimumSizeHint());
    // Keep receiving motion even when the pointer outruns the window or crosses
    // a native child.  startSystemResize() would have had the WM guarantee this;
    // driving the drag ourselves means asking for the grab ourselves.  A failed
    // grab isn't fatal — motion events still arrive while the pointer stays
    // over one of our own windows — but a fast drag can then outrun the window
    // and stall, so it's worth a log line to explain a report of that.
    m_manualResizeGrabbed = false;
    if (QWindow* handle = m_window->windowHandle()) {
        m_manualResizeGrabbed = handle->setMouseGrabEnabled(true);
        if (!m_manualResizeGrabbed) {
            qCWarning(lcGui) << "FramelessResizer: mouse grab denied for manual "
                                 "resize; drag may stall if the pointer outruns "
                                 "the window";
        }
    }
}

// static
QRect FramelessResizer::clampManualResize(const QRect& startGeom, const QPoint& delta,
                                           Qt::Edges edges, const QSize& floor,
                                           const QSize& ceiling)
{
    QRect geom = startGeom;

    // The effective floor is the explicit minimum *and* whatever the layout
    // demands.  Clamping on minimumWidth()/minimumHeight() alone lets us ask
    // for a size Qt then silently grows back, and because it grows the size
    // while we keep moving the origin, the edge we are supposed to be holding
    // still drifts.  `floor` is snapshotted once in beginManualResize()
    // rather than recomputed here on every motion event.
    const int minW = floor.width();
    const int minH = floor.height();
    const int maxW = ceiling.width();
    const int maxH = ceiling.height();

    if (edges & Qt::LeftEdge) {
        int newLeft = geom.left() + delta.x();
        newLeft = std::min(newLeft, geom.right() - minW + 1);
        newLeft = std::max(newLeft, geom.right() - maxW + 1);
        geom.setLeft(newLeft);
    }
    if (edges & Qt::RightEdge) {
        int newRight = geom.right() + delta.x();
        newRight = std::max(newRight, geom.left() + minW - 1);
        newRight = std::min(newRight, geom.left() + maxW - 1);
        geom.setRight(newRight);
    }
    if (edges & Qt::TopEdge) {
        int newTop = geom.top() + delta.y();
        newTop = std::min(newTop, geom.bottom() - minH + 1);
        newTop = std::max(newTop, geom.bottom() - maxH + 1);
        geom.setTop(newTop);
    }
    if (edges & Qt::BottomEdge) {
        int newBottom = geom.bottom() + delta.y();
        newBottom = std::max(newBottom, geom.top() + minH - 1);
        newBottom = std::min(newBottom, geom.top() + maxH - 1);
        geom.setBottom(newBottom);
    }
    return geom;
}

void FramelessResizer::continueManualResize(const QPoint& globalPos)
{
    const QPoint delta = globalPos - m_manualResizePressGlobal;
    const QSize ceiling(m_window->maximumWidth(), m_window->maximumHeight());
    const QRect geom = clampManualResize(m_manualResizeStartGeom, delta,
                                          m_manualResizeEdges, m_manualResizeFloor,
                                          ceiling);

    // Skip no-op requests: several motion events can map to the same geometry,
    // and there is no reason to ask the platform to apply a geometry it is
    // already at.
    if (geom == m_manualResizeLastRequested) {
        return;
    }
    m_manualResizeLastRequested = geom;
    m_window->setGeometry(geom);
}

void FramelessResizer::endManualResize()
{
    if (!m_manualResizeActive) {
        return;
    }
    m_manualResizeActive = false;
    m_manualResizeGrabbed = false;
    m_manualResizeEdges = {};
    if (QWindow* handle = m_window->windowHandle()) {
        handle->setMouseGrabEnabled(false);
    }
    leaveEdgeZone();
}

bool FramelessResizer::eventFilter(QObject* obj, QEvent* ev)
{
    switch (ev->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::Leave:
        break;
    default:
        return false;   // the overwhelmingly common path
    }

    // Window-level events only: widget-level MouseMove is gated on the widget
    // having opted into mouse tracking, so it can't be relied on for hover.
    // While a manual drag genuinely holds the grab, every event in it is ours
    // by definition — the pointer may well be outside the window by then —
    // but only once that grab actually succeeded (m_manualResizeGrabbed); a
    // denied grab gives us no exclusivity guarantee, so without it we still
    // require ownsWindow() like any other event, accepting that a fast drag
    // may then stall if the pointer outruns the window (beginManualResize()
    // already warns about that case).
    auto* win = qobject_cast<QWindow*>(obj);
    if (!win) {
        return false;
    }
    if (!(m_manualResizeActive && m_manualResizeGrabbed) && !ownsWindow(win)) {
        // Pointer activity somewhere else while our override cursor is still
        // up: a popup that opened under a stationary pointer takes the events
        // without us ever seeing the move that would have cleared it.  The
        // override cursor is application-global, so drop it here rather than
        // strand a resize cursor over an unrelated window.
        if (m_cursorOverridden && ev->type() == QEvent::MouseMove) {
            leaveEdgeZone();
        }
        return false;
    }

    // Native decorations: the OS handles resize. endManualResize() releases a
    // grab/active flag left by a mid-drag frameless toggle (that flag bypasses
    // the ownership check). leaveEdgeZone() is still needed: endManualResize()
    // returns early when no drag is active, the hover-only case.
    if (!(m_window->windowFlags() & Qt::FramelessWindowHint)) {
        endManualResize();
        leaveEdgeZone();
        return false;
    }

    switch (ev->type()) {

    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (m_manualResizeActive) {
            // Require both the event's snapshot and the live state to agree the
            // button is up.  Moving the window from continueManualResize() makes
            // Qt deliver follow-up moves that can report no buttons held, and
            // acting on one of those alone strands the resize part way through;
            // the live state alone can equally go stale and never end the drag.
            // A genuine release ends it through MouseButtonRelease below — this
            // is only the backstop for one we never see, or a WM-broken grab.
            if (!(me->buttons() & Qt::LeftButton)
                && !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
                endManualResize();
                break;
            }
            // QCursor::pos(), not the event's globalPosition(): a left/top drag
            // moves the window under the pointer, and Qt derives that field
            // from a local coordinate against an origin we have just changed.
            // It therefore trails the real pointer, and feeding it back in
            // makes the anchored edge visibly wobble and the drag settle short.
            continueManualResize(QCursor::pos());
            return true;
        }
        if (me->buttons() != Qt::NoButton) break;
        const Qt::Edges edges =
            edgesAt(m_window->mapFromGlobal(me->globalPosition().toPoint()));
        if (edges) {
            enterEdgeZone(edges);
        } else {
            leaveEdgeZone();
        }
        break;
    }

    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() != Qt::LeftButton) break;
        // The event's own globalPosition(), not QCursor::pos(): this runs on
        // every platform, including native Wayland where a global cursor-
        // position query is a documented compositor limitation Qt can't fully
        // paper over — a stale read here would compute the wrong edges (or
        // none) for a resize that, unlike the xcb path below, actually goes
        // through the platform's startSystemResize().  No lag risk taking it
        // from the event: the window hasn't moved yet at press time, which is
        // the only reason continueManualResize() needs QCursor::pos() instead.
        const QPoint global = me->globalPosition().toPoint();
        const Qt::Edges edges = edgesAt(m_window->mapFromGlobal(global));
        if (edges && m_window->windowHandle()) {
            if (FramelessMoveHelper::systemMoveResizeUnreliable(m_window)) {
                // xcb (or Windows+translucent): startSystemResize() would
                // report success but the WM-driven grab it hands off to
                // doesn't actually work (QTBUG-69716 / QTBUG-90628) — drag
                // the geometry ourselves instead, the same fallback Qt's own
                // QSizeGrip uses. The edge cursor stays up for the duration
                // since the WM isn't showing one.
                beginManualResize(edges, global);
            } else {
                // macOS QCocoaWindow::startSystemResize() always returns false,
                // so fall back to the manual drag (keeping our cursor) and only
                // hand the cursor to the OS once its grab took the resize.
                if (m_window->windowHandle()->startSystemResize(edges)) {
                    leaveEdgeZone();
                } else {
                    beginManualResize(edges, global);
                }
            }
            return true;      // consume: no widget should receive this press
        }
        break;
    }

    case QEvent::MouseButtonRelease: {
        // Only the button that started the drag ends it — a right-click part
        // way through shouldn't abort the resize (and get swallowed doing it).
        auto* me = static_cast<QMouseEvent*>(ev);
        if (m_manualResizeActive && me->button() == Qt::LeftButton) {
            // Settle on the release position, since the final move can be
            // coalesced away under load. QCursor::pos(), not globalPosition():
            // during a left/top drag Qt derives that from an origin we just
            // moved, so it lags a motion event.
            continueManualResize(QCursor::pos());
            endManualResize();
            return true;
        }
        break;
    }

    case QEvent::Leave:
        if (shouldEndOnUngrabbedLeave(m_manualResizeActive, m_manualResizeGrabbed)) {
            // Denied grab: once the pointer leaves, ownsWindow() filters out
            // every event for us, including the release. Left active, a later
            // press-and-return would resume from a stale
            // m_manualResizePressGlobal and snap the window. The ungrabbed path
            // is best-effort anyway (see beginManualResize()).
            endManualResize();
            break;
        }
        // Drop the cursor only once the pointer has really left: moving between
        // native children also raises Leave. Deferred one turn because
        // WA_UnderMouse is updated by the widget's event() after app-level
        // filters run, so underMouse() is stale inline (QCursor::pos() is
        // unreliable on Wayland). `this` as context drops the lambda if the
        // resizer dies first.
        if (!m_manualResizeActive) {
            QTimer::singleShot(0, this, [this]() {
                if (!m_manualResizeActive && m_window && !m_window->underMouse()) {
                    leaveEdgeZone();
                }
            });
        }
        break;

    default:
        break;
    }
    return false;
}

} // namespace AetherSDR
