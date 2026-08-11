#include "gui/workspace/WorkspaceCanvas.h"

#include "gui/workspace/CanvasItemFrame.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QResizeEvent>

#include <algorithm>

namespace AetherSDR {

WorkspaceCanvas::WorkspaceCanvas(QWidget* parent)
    : QWidget(parent)
{
    // Children are positioned absolutely from the model, so the canvas must
    // have no layout manager: one would fight applyGeometry() on every
    // resize and win.
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);   // keyboard placement (phase 5)
    setAccessibleName(QStringLiteral("Workspace canvas"));

    m_frame = new CanvasItemFrame(this);
}

WorkspaceCanvas::~WorkspaceCanvas()
{
    // Destruction order is the trap here.  C++ destroys this class's members
    // (m_layout, m_widgets) before the ~QWidget base runs, and ~QWidget is
    // what deletes the child widgets — each of which emits destroyed().  A
    // still-connected watch would therefore run its lambda against a
    // CanvasLayout that no longer exists.  Qt's usual context-object
    // protection does not save us: it disconnects in ~QObject, which is
    // further down the chain than ~QWidget.
    //
    // So sever them here, while everything is still alive.
    for (const auto& widget : m_widgets) {
        if (QWidget* w = widget.data()) {
            disconnect(w, &QObject::destroyed, this, nullptr);
        }
    }
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

    if (!m_layout.addItem(item)) {
        return false;
    }

    content->setParent(this);
    content->installEventFilter(this);
    m_widgets.insert(id, content);

    // If the widget dies without going through takeItem()/removeItem() — a
    // parent destroyed out from under us, or content someone else owns — the
    // QPointer nulls but the model entry would survive as a ghost: contains()
    // and itemCount() would still report it, hitTest() would still return its
    // id over dead space, and addItem() with that id would fail forever.
    // Phase 3 moves applets between canvases and containers, which is exactly
    // where that happens (PR #4900 review).
    connect(content, &QObject::destroyed, this, [this, id] {
        if (!m_layout.contains(id)) {
            return;
        }
        m_widgets.remove(id);
        m_layout.removeItem(id);
        applyStacking();
        emit itemRemoved(id);
    });

    applyGeometryFor(id);
    content->show();
    applyStacking();

    emit itemAdded(id);
    emit itemRectChanged(id, itemRect(id));
    return true;
}

int WorkspaceCanvas::restoreItems(const QList<CanvasItem>& items,
                                  const QHash<QString, QWidget*>& widgets)
{
    // Sort by stored z and place bottom-to-top, which is what makes the saved
    // stacking survive — see CanvasLayout::restoreItems() for why the batch
    // shape is the only one that cannot get this wrong.
    QList<CanvasItem> ordered = items;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const CanvasItem& a, const CanvasItem& b) {
                         return a.z < b.z;
                     });

    int placed = 0;
    for (const CanvasItem& item : ordered) {
        QWidget* content = widgets.value(item.id);
        if (!content) {
            continue;   // nothing to show for this id; skip rather than guess
        }
        if (addItem(item.id, content, item.rect, item.contentType,
                    item.minimumSize)) {
            ++placed;
        }
    }
    return placed;
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

    if (id == m_selectedId) {
        clearSelection();
    }
    if (id == m_gestureId) {
        // The item is leaving mid-gesture (eviction, disable) — drop the
        // session without touching a rect that no longer matters.
        m_gestureId.clear();
        m_vGuides.clear();
        m_hGuides.clear();
        releaseKeyboard();
        update();
    }

    QWidget* w = m_widgets.take(id).data();
    m_layout.removeItem(id);

    if (w) {
        w->removeEventFilter(this);
        // Drop the destroyed-watch with the widget, or a later destruction
        // would fire a lambda still holding this id — and if something else
        // has since taken the id, it would evict the wrong item.
        disconnect(w, &QObject::destroyed, this, nullptr);
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
    if (!m_layout.setRect(id, rect)) {
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

void WorkspaceCanvas::setDropMimeType(const QByteArray& mimeType)
{
    m_dropMimeType = mimeType;
    setAcceptDrops(!mimeType.isEmpty());
}

void WorkspaceCanvas::dragEnterEvent(QDragEnterEvent* ev)
{
    if (!m_dropMimeType.isEmpty()
        && ev->mimeData()->hasFormat(QString::fromLatin1(m_dropMimeType))) {
        ev->acceptProposedAction();
    }
}

void WorkspaceCanvas::dragMoveEvent(QDragMoveEvent* ev)
{
    if (!m_dropMimeType.isEmpty()
        && ev->mimeData()->hasFormat(QString::fromLatin1(m_dropMimeType))) {
        ev->acceptProposedAction();
    }
}

void WorkspaceCanvas::dropEvent(QDropEvent* ev)
{
    if (m_dropMimeType.isEmpty()
        || !ev->mimeData()->hasFormat(QString::fromLatin1(m_dropMimeType))
        || width() <= 0 || height() <= 0) {
        return;
    }

    const QString payload = QString::fromUtf8(
        ev->mimeData()->data(QString::fromLatin1(m_dropMimeType)));
    const QPointF pos(ev->position().x() / width(),
                      ev->position().y() / height());

    ev->acceptProposedAction();
    emit dropReceived(payload, pos);
}

void WorkspaceCanvas::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);

    // Recompute display geometry only.  The model is untouched and nothing
    // is announced: a resize is not an edit, and persisting what a small
    // window forced on the view is how a transient startup size once
    // destroyed a stored arrangement (RFC #4887 phase 3 field report).
    applyGeometry();
}

bool WorkspaceCanvas::eventFilter(QObject* watched, QEvent* ev)
{
    if (ev->type() == QEvent::MouseButtonPress) {
        for (auto it = m_widgets.constBegin(); it != m_widgets.constEnd(); ++it) {
            if (it.value().data() == watched) {
                // bringItemToFront() is false when the item was already
                // frontmost, and then nothing is emitted.  Without that, every
                // click anywhere on the canvas would look like an edit and
                // cost a whole-document write after the debounce — the same
                // rule resizeEvent() follows (PR #4900 review, M2).
                bringItemToFront(it.key());
                selectItem(it.key());
                break;
            }
        }
    }
    // Never consumed: raising is incidental to whatever the press was for.
    return QWidget::eventFilter(watched, ev);
}

QRect WorkspaceCanvas::displayRectFor(const QString& id) const
{
    const CanvasItem* it = m_layout.item(id);
    if (!it) {
        return {};
    }
    // The display clamp lives HERE and only here: minimum sizes are enforced
    // against the canvas as it is right now, the stored rect stays pristine,
    // and a canvas too small to honour a minimum shows a compromise it will
    // abandon the moment the canvas grows back.
    return toPixels(clampToCanvas(it->rect, it->minimumSize, size()), size());
}

void WorkspaceCanvas::applyGeometryFor(const QString& id)
{
    QWidget* w = m_widgets.value(id).data();
    if (!w || !m_layout.contains(id)) {
        return;
    }
    w->setGeometry(displayRectFor(id));
    if (id == m_selectedId) {
        updateFrame();
    }
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
    // The selection frame rides above every item, always.
    if (m_frame && m_frame->isVisible()) {
        m_frame->raise();
    }
}

// ── Selection (phase 5) ──────────────────────────────────────────────────

void WorkspaceCanvas::selectItem(const QString& id)
{
    if (id == m_selectedId || !m_layout.contains(id)) {
        return;
    }
    m_selectedId = id;
    updateFrame();
    if (QWidget* w = m_widgets.value(id).data()) {
        setAccessibleDescription(
            QStringLiteral("%1 selected").arg(w->accessibleName().isEmpty()
                                                  ? id
                                                  : w->accessibleName()));
    }
    emit selectionChanged(id);
}

void WorkspaceCanvas::clearSelection()
{
    if (m_selectedId.isEmpty()) {
        return;
    }
    m_selectedId.clear();
    updateFrame();
    setAccessibleDescription(QString());
    emit selectionChanged(QString());
}

void WorkspaceCanvas::updateFrame()
{
    if (!m_frame) {
        return;
    }
    if (m_selectedId.isEmpty() || !m_layout.contains(m_selectedId)) {
        m_frame->hide();
        return;
    }
    m_frame->followItem(displayRectFor(m_selectedId));
    m_frame->show();
    m_frame->raise();
}

// ── The gesture session (phase 5) ────────────────────────────────────────

QSizeF WorkspaceCanvas::minNormFor(const QString& id) const
{
    const CanvasItem* it = m_layout.item(id);
    if (!it) {
        return {};
    }
    return minimumNormSize(it->minimumSize, size());
}

void WorkspaceCanvas::beginGesture(const QString& id, HitZone zone,
                                   const QPoint& globalPos)
{
    const CanvasItem* it = m_layout.item(id);
    if (!it || zone == HitZone::None || width() <= 0 || height() <= 0) {
        return;
    }
    if (gestureActive()) {
        cancelGesture();   // one at a time; a stray second press wins cleanly
    }

    m_gestureId     = id;
    m_gestureZone   = zone;
    m_gestureStart  = it->rect;
    m_gestureOrigin = globalPos;

    selectItem(id);
    // Esc-to-cancel has to work wherever focus happens to be, including
    // mid-drag from a title bar that has the mouse grab.  Scoped strictly
    // to the gesture: released in endGesture()/cancelGesture().
    grabKeyboard();
    emit gestureStarted(id, m_gestureStart);
}

void WorkspaceCanvas::beginMoveGesture(const QString& id, const QPoint& globalPos)
{
    beginGesture(id, HitZone::Move, globalPos);
}

void WorkspaceCanvas::beginFrameGesture(HitZone zone, const QPoint& globalPos)
{
    if (!m_selectedId.isEmpty()) {
        beginGesture(m_selectedId, zone, globalPos);
    }
}

void WorkspaceCanvas::moveGesture(const QPoint& globalPos)
{
    if (!gestureActive() || width() <= 0 || height() <= 0) {
        return;
    }

    const QPoint deltaPx = globalPos - m_gestureOrigin;
    const double dx = deltaPx.x() / double(width());
    const double dy = deltaPx.y() / double(height());

    const QSizeF minN = minNormFor(m_gestureId);
    NormRect r = applyDrag(m_gestureStart, m_gestureZone, dx, dy, minN);

    m_vGuides.clear();
    m_hGuides.clear();
    // Alt suppresses snapping for as long as it is held — checked per
    // motion so the operator can toggle precision mid-drag.
    if (!(QApplication::keyboardModifiers() & Qt::AltModifier)) {
        QList<NormRect> peers;
        for (const CanvasItem& other : m_layout.itemsByZ()) {
            if (other.id != m_gestureId) {
                peers.append(other.rect);
            }
        }
        const SnapResult snapped =
            snapRect(r, m_gestureZone, peers,
                     kSnapTolerancePx / double(width()),
                     kSnapTolerancePx / double(height()), minN);
        r         = snapped.rect;
        m_vGuides = snapped.verticalGuides;
        m_hGuides = snapped.horizontalGuides;
    }

    setItemRect(m_gestureId, r);
    update();   // guides
}

void WorkspaceCanvas::endGesture(const QPoint& globalPos)
{
    if (!gestureActive()) {
        return;
    }
    const QString id   = m_gestureId;
    const HitZone zone = m_gestureZone;
    m_gestureId.clear();
    m_vGuides.clear();
    m_hGuides.clear();
    releaseKeyboard();
    update();

    // A MOVE released outside the canvas is not a placement — it is either a
    // return to the panel (the controller's call) or an abort.  Restore the
    // start rect first so no path leaves the item where it cannot live.
    if (zone == HitZone::Move && !rect().contains(mapFromGlobal(globalPos))) {
        setItemRect(id, m_gestureStart);
        emit itemDraggedOut(id, globalPos);
        return;
    }
    emit gestureFinished(id);
}

void WorkspaceCanvas::cancelGesture()
{
    if (!gestureActive()) {
        return;
    }
    const QString id = m_gestureId;
    m_gestureId.clear();
    m_vGuides.clear();
    m_hGuides.clear();
    releaseKeyboard();
    setItemRect(id, m_gestureStart);
    update();
}

// ── Bare-canvas input (phase 5) ──────────────────────────────────────────

void WorkspaceCanvas::mousePressEvent(QMouseEvent* ev)
{
    // Reached two ways: a genuine bare-canvas press, and a press on a
    // NON-INTERACTIVE part of an item whose widget ignored it — Qt
    // propagates ignored presses to the parent.  Deselecting on the second
    // kind would undo the selection the event filter just made (the click
    // landed ON the item, from the operator's point of view), so hit-test
    // rather than assume.
    const QString hit = hitTest(ev->position().toPoint());
    if (hit.isEmpty()) {
        clearSelection();
    } else {
        bringItemToFront(hit);
        selectItem(hit);   // no-op when the filter already selected it
    }
    setFocus(Qt::MouseFocusReason);
    QWidget::mousePressEvent(ev);
}

bool WorkspaceCanvas::event(QEvent* ev)
{
    // The main window binds the arrow family as application shortcuts —
    // frequency nudges on Left/Right and Shift+Left/Right — and application
    // shortcuts consume key events before the focused widget sees them.
    // Accepting the ShortcutOverride reclaims the key for the widget.
    //
    // Scope is deliberate and narrow: only while a canvas item is SELECTED
    // (or a gesture is live) and only while the canvas itself has focus —
    // the override is consulted for the focus widget alone, so clicking into
    // the spectrum or deselecting hands the VFO its keys straight back.
    // Selecting an item is the operator saying "I'm placing things now."
    if (ev->type() == QEvent::ShortcutOverride
        && (!m_selectedId.isEmpty() || gestureActive())) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        switch (ke->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Escape:
            ke->accept();
            return true;
        default:
            break;
        }
    }
    return QWidget::event(ev);
}

void WorkspaceCanvas::keyPressEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Escape) {
        if (gestureActive()) {
            cancelGesture();
        } else {
            clearSelection();
        }
        ev->accept();
        return;
    }

    // Keyboard placement: arrows nudge the selected item, Shift+arrows
    // resize it (SE-corner semantics: right/bottom edges move), Ctrl makes
    // either fine-grained.  Deliberately no snapping — reaching for the
    // keyboard is a statement of precise intent.
    const bool arrow = ev->key() == Qt::Key_Left || ev->key() == Qt::Key_Right
                       || ev->key() == Qt::Key_Up || ev->key() == Qt::Key_Down;
    if (!arrow || m_selectedId.isEmpty() || gestureActive()
        || width() <= 0 || height() <= 0) {
        QWidget::keyPressEvent(ev);
        return;
    }

    const int px = (ev->modifiers() & Qt::ControlModifier) ? 1 : kNudgePx;
    const double dx = (ev->key() == Qt::Key_Right ? px
                       : ev->key() == Qt::Key_Left ? -px : 0) / double(width());
    const double dy = (ev->key() == Qt::Key_Down ? px
                       : ev->key() == Qt::Key_Up ? -px : 0) / double(height());

    const CanvasItem* it = m_layout.item(m_selectedId);
    if (!it) {
        return;
    }
    const HitZone zone = (ev->modifiers() & Qt::ShiftModifier)
                             ? HitZone::SE
                             : HitZone::Move;
    const NormRect before = it->rect;
    const NormRect after =
        applyDrag(before, zone, dx, dy, minNormFor(m_selectedId));

    emit gestureStarted(m_selectedId, before);
    setItemRect(m_selectedId, after);
    emit gestureFinished(m_selectedId);
    ev->accept();
}

void WorkspaceCanvas::paintEvent(QPaintEvent* ev)
{
    QWidget::paintEvent(ev);
    if (m_vGuides.isEmpty() && m_hGuides.isEmpty()) {
        return;
    }
    QPainter p(this);
    QPen pen(palette().highlight().color(), 1, Qt::DashLine);
    p.setPen(pen);
    for (double x : m_vGuides) {
        const int px = qRound(x * width());
        p.drawLine(px, 0, px, height());
    }
    for (double y : m_hGuides) {
        const int py = qRound(y * height());
        p.drawLine(0, py, width(), py);
    }
}

void WorkspaceCanvas::contextMenuEvent(QContextMenuEvent* ev)
{
    emit contextMenuRequested(hitTest(ev->pos()), ev->globalPos());
    ev->accept();
}

}  // namespace AetherSDR
