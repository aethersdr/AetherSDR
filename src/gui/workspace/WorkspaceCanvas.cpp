#include "gui/workspace/WorkspaceCanvas.h"

#include <QEvent>
#include <QMouseEvent>
#include <QResizeEvent>

namespace AetherSDR {

WorkspaceCanvas::WorkspaceCanvas(QWidget* parent)
    : QWidget(parent)
{
    // Children are positioned absolutely from the model, so the canvas must
    // have no layout manager: one would fight applyGeometry() on every
    // resize and win.
    setMouseTracking(true);
}

bool WorkspaceCanvas::addItem(const QString& id,
                              QWidget* content,
                              const NormRect& rect,
                              const QString& contentType,
                              const QSize& minimumSize)
{
    if (!content || id.isEmpty() || m_layout.contains(id)) {
        return false;
    }

    CanvasItem item;
    item.id          = id;
    item.contentType = contentType;
    item.rect        = rect;
    item.minimumSize = minimumSize;

    if (!m_layout.addItem(item, size())) {
        return false;
    }

    content->setParent(this);
    content->installEventFilter(this);
    m_widgets.insert(id, content);

    applyGeometryFor(id);
    content->show();
    applyStacking();

    emit itemAdded(id);
    emit itemRectChanged(id, itemRect(id));
    return true;
}

bool WorkspaceCanvas::removeItem(const QString& id)
{
    QWidget* w = takeItem(id);
    if (!w) {
        return false;
    }
    w->deleteLater();
    return true;
}

QWidget* WorkspaceCanvas::takeItem(const QString& id)
{
    if (!m_layout.contains(id)) {
        return nullptr;
    }

    QWidget* w = m_widgets.take(id).data();
    m_layout.removeItem(id);

    if (w) {
        w->removeEventFilter(this);
        w->hide();
        w->setParent(nullptr);
    }

    // Removal renumbers z, so the surviving items' stacking has to be redone.
    applyStacking();
    emit itemRemoved(id);
    return w;
}

QWidget* WorkspaceCanvas::itemWidget(const QString& id) const
{
    return m_widgets.value(id).data();
}

bool WorkspaceCanvas::setItemRect(const QString& id, const NormRect& rect)
{
    if (!m_layout.setRect(id, rect, size())) {
        return false;
    }
    applyGeometryFor(id);
    emit itemRectChanged(id, itemRect(id));
    return true;
}

NormRect WorkspaceCanvas::itemRect(const QString& id) const
{
    const CanvasItem* it = m_layout.item(id);
    return it ? it->rect : NormRect{};
}

QString WorkspaceCanvas::hitTest(const QPoint& pos) const
{
    if (width() <= 0 || height() <= 0) {
        return {};
    }
    return m_layout.hitTest(QPointF(pos.x() / static_cast<double>(width()),
                                    pos.y() / static_cast<double>(height())));
}

bool WorkspaceCanvas::raiseItem(const QString& id)
{
    if (!m_layout.raise(id)) {
        return false;
    }
    applyStacking();
    emit itemStackingChanged(id);
    return true;
}

bool WorkspaceCanvas::lowerItem(const QString& id)
{
    if (!m_layout.lower(id)) {
        return false;
    }
    applyStacking();
    emit itemStackingChanged(id);
    return true;
}

bool WorkspaceCanvas::bringItemToFront(const QString& id)
{
    if (!m_layout.bringToFront(id)) {
        return false;
    }
    applyStacking();
    emit itemStackingChanged(id);
    return true;
}

bool WorkspaceCanvas::sendItemToBack(const QString& id)
{
    if (!m_layout.sendToBack(id)) {
        return false;
    }
    applyStacking();
    emit itemStackingChanged(id);
    return true;
}

void WorkspaceCanvas::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);

    // A smaller canvas can push items out or below their minimum, so the
    // model is re-clamped first and the pixels follow.  Only the items that
    // actually moved are announced — a resize that changes nothing must not
    // look like an edit (this is what keeps phase 2's auto-commit from
    // writing the document on every frame of a window drag).
    const QStringList moved = m_layout.reclampAll(size());
    applyGeometry();
    for (const QString& id : moved) {
        emit itemRectChanged(id, itemRect(id));
    }
}

void WorkspaceCanvas::mousePressEvent(QMouseEvent* ev)
{
    // Only reached for presses on bare canvas: anything over an item goes to
    // that item's widget, and comes back through eventFilter().
    QWidget::mousePressEvent(ev);
}

bool WorkspaceCanvas::eventFilter(QObject* watched, QEvent* ev)
{
    if (ev->type() == QEvent::MouseButtonPress) {
        for (auto it = m_widgets.constBegin(); it != m_widgets.constEnd(); ++it) {
            if (it.value().data() == watched) {
                bringItemToFront(it.key());
                break;
            }
        }
    }
    // Never consumed: raising is incidental to whatever the press was for.
    return QWidget::eventFilter(watched, ev);
}

void WorkspaceCanvas::applyGeometryFor(const QString& id)
{
    QWidget* w = m_widgets.value(id).data();
    const CanvasItem* it = m_layout.item(id);
    if (!w || !it) {
        return;
    }
    w->setGeometry(toPixels(it->rect, size()));
}

void WorkspaceCanvas::applyGeometry()
{
    for (const CanvasItem& it : m_layout.itemsByZ()) {
        applyGeometryFor(it.id);
    }
}

void WorkspaceCanvas::applyStacking()
{
    // Bottom-to-top: each raise() puts that widget above everything raised so
    // far, so the last one — the highest z — ends up on top.
    for (const CanvasItem& it : m_layout.itemsByZ()) {
        if (QWidget* w = m_widgets.value(it.id).data()) {
            w->raise();
        }
    }
}

}  // namespace AetherSDR
