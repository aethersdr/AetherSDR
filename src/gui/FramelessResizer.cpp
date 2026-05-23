#include "FramelessResizer.h"

#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>

namespace MasterSDR {

void FramelessResizer::install(QWidget* window, int margin)
{
    new FramelessResizer(window, margin);
}

FramelessResizer::FramelessResizer(QWidget* window, int margin)
    : QObject(window), m_window(window), m_margin(margin)
{
    if (window->windowHandle()) {
        // Native window already exists — install directly on it.
        window->windowHandle()->installEventFilter(this);
    } else {
        // Native window not yet created (called from constructor before
        // show()).  Install on the QWidget temporarily and migrate to the
        // QWindow when WinIdChange fires.
        window->installEventFilter(this);
    }
}

FramelessResizer::~FramelessResizer()
{
    // The override cursor is global state.  If we're destroyed while the
    // cursor is currently overridden (window closed while hovering an
    // edge, or fast cursor flick between windows that skipped the matching
    // leaveEdgeZone), restore it now to avoid leaking the resize cursor
    // onto the desktop or other windows.
    leaveEdgeZone();
}

Qt::Edges FramelessResizer::edgesAt(const QPoint& p) const
{
    const QRect r = m_window->rect();
    Qt::Edges edges;
    if (p.x() <= m_margin)              edges |= Qt::LeftEdge;
    if (p.x() >= r.width()  - m_margin) edges |= Qt::RightEdge;
    if (p.y() <= m_margin)              edges |= Qt::TopEdge;
    if (p.y() >= r.height() - m_margin) edges |= Qt::BottomEdge;
    return edges;
}

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

bool FramelessResizer::eventFilter(QObject* obj, QEvent* ev)
{
    // ── Phase 1: QWidget (pre-native-window) ─────────────────────────────
    // Waiting for the native window to be created.  Once WinIdChange fires,
    // migrate the filter to the QWindow and stay there.
    if (obj == m_window) {
        if (ev->type() == QEvent::WinIdChange && m_window->windowHandle()) {
            m_window->removeEventFilter(this);
            m_window->windowHandle()->installEventFilter(this);
        }
        return false;
    }

    // ── Phase 2: QWindow (native handle) ─────────────────────────────────
    // Hands off when the window has native decorations — the OS handles resize.
    if (!(m_window->windowFlags() & Qt::FramelessWindowHint)) {
        leaveEdgeZone();
        return false;
    }

    switch (ev->type()) {

    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->buttons() != Qt::NoButton) break;
        // QWindow delivers mouse positions in window-local coordinates.
        const Qt::Edges edges = edgesAt(me->position().toPoint());
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
        const Qt::Edges edges = edgesAt(me->position().toPoint());
        if (edges && m_window->windowHandle()) {
            leaveEdgeZone();  // hand cursor control back to the OS
            m_window->windowHandle()->startSystemResize(edges);
            return true;      // consume: no widget should receive this press
        }
        break;
    }

    case QEvent::Leave:
        leaveEdgeZone();
        break;

    default:
        break;
    }
    return false;
}

} // namespace MasterSDR
