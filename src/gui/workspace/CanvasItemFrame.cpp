#include "gui/workspace/CanvasItemFrame.h"

#include "gui/workspace/WorkspaceCanvas.h"

#include <QMouseEvent>
#include <QPainter>
#include <QRegion>

namespace AetherSDR {

CanvasItemFrame::CanvasItemFrame(WorkspaceCanvas* canvas)
    : QWidget(canvas)
    , m_canvas(canvas)
{
    setMouseTracking(true);   // hover cursors without a button down
    hide();
}

void CanvasItemFrame::followItem(const QRect& itemRectPx)
{
    setGeometry(itemRectPx);

    // Everything except the border band is cut out of the widget: paints
    // nothing there, receives nothing there.  This is what keeps the
    // applet interactive while selected.
    QRegion band(rect());
    band -= rect().adjusted(kGripPx, kGripPx, -kGripPx, -kGripPx);
    setMask(band);
}

HitZone CanvasItemFrame::zoneAt(const QPoint& localPos) const
{
    // titleHPx = 0: moving is the title bar's job (and the pan stack, which
    // has no title bar, is deliberately resize-only — its position is fully
    // determined by dragging its four edges).
    return hitZoneFor(rect(), localPos, kGripPx, /*titleHPx=*/0);
}

void CanvasItemFrame::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QColor accent = palette().highlight().color();

    // The band outline…
    p.setPen(QPen(accent, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // …and the eight grips: corners plus edge midpoints, filled squares the
    // size of the band so what you can grab is exactly what you can see.
    p.setBrush(accent);
    const int g = kGripPx;
    const int w = width(), h = height();
    const QList<QPoint> grips{
        {0, 0},           {w - g, 0},           {0, h - g},       {w - g, h - g},
        {(w - g) / 2, 0}, {(w - g) / 2, h - g}, {0, (h - g) / 2}, {w - g, (h - g) / 2},
    };
    for (const QPoint& at : grips) {
        p.drawRect(QRect(at, QSize(g - 1, g - 1)));
    }
}

void CanvasItemFrame::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(ev);
        return;
    }
    const HitZone zone = zoneAt(ev->position().toPoint());
    if (!isResizeZone(zone)) {
        return;
    }
    m_dragging = true;
    m_canvas->beginFrameGesture(zone, ev->globalPosition().toPoint());
    ev->accept();
}

void CanvasItemFrame::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_dragging) {
        m_canvas->moveGesture(ev->globalPosition().toPoint());
        ev->accept();
        return;
    }
    setCursor(cursorForZone(zoneAt(ev->position().toPoint())));
    QWidget::mouseMoveEvent(ev);
}

void CanvasItemFrame::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_dragging && ev->button() == Qt::LeftButton) {
        m_dragging = false;
        m_canvas->endGesture(ev->globalPosition().toPoint());
        ev->accept();
        return;
    }
    QWidget::mouseReleaseEvent(ev);
}

}  // namespace AetherSDR
