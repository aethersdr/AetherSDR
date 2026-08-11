#include "gui/workspace/WorkspaceController.h"

#include "gui/containers/ContainerManager.h"
#include "gui/containers/ContainerWidget.h"
#include "gui/workspace/CanvasInteraction.h"
#include "gui/workspace/ClassicLayout.h"
#include "gui/workspace/WorkspaceMigration.h"
#include "gui/workspace/WorkspaceCanvas.h"

#include <QHash>
#include <QMenu>
#include <QPoint>
#include <QWidget>

namespace AetherSDR {

namespace {

const QString kAppletItemPrefix = QStringLiteral("applet:");

// The MIME type the panel's title-bar drags already carry (#3057); the
// canvas accepts the same one so place/move/return are all one mechanism.
const QByteArray kAppletMime = QByteArrayLiteral("application/x-aethersdr-applet");

}  // namespace

const QString WorkspaceController::kPanStackItemId = QStringLiteral("panstack");

WorkspaceController::WorkspaceController(ContainerManager* manager,
                                         WorkspaceCanvas* canvas,
                                         QObject* parent)
    : QObject(parent)
    , m_manager(manager)
    , m_canvas(canvas)
{
    Q_ASSERT(m_manager);
    Q_ASSERT(m_canvas);

    m_canvas->setDropMimeType(kAppletMime);

    connect(m_canvas, &WorkspaceCanvas::dropReceived,
            this, &WorkspaceController::onDropReceived);
    connect(m_canvas, &WorkspaceCanvas::itemRectChanged,
            this, &WorkspaceController::onItemRectChanged);
    connect(m_canvas, &WorkspaceCanvas::itemStackingChanged,
            this, [this](const QString&) {
                if (m_applying || !m_enabled) return;
                writeStackingFromCanvas();
            });

    // Gestures (phase 5): snapshot for undo at the start, flush the debounced
    // rect stream at the end — the auto-commit gesture boundary.
    connect(m_canvas, &WorkspaceCanvas::gestureStarted,
            this, [this](const QString& itemId, const NormRect& startRect) {
                if (m_applying || !m_enabled) return;
                m_undoItemId = itemId;
                m_undoRect   = startRect;
            });
    connect(m_canvas, &WorkspaceCanvas::gestureFinished,
            this, [this](const QString&) {
                if (m_applying || !m_enabled) return;
                m_store.flush();
            });
    connect(m_canvas, &WorkspaceCanvas::itemDraggedOut,
            this, &WorkspaceController::onItemDraggedOut);
    connect(m_canvas, &WorkspaceCanvas::contextMenuRequested,
            this, &WorkspaceController::onContextMenuRequested);

    // Leaving the canvas through the manager — the title-bar "return to
    // panel" button, or the detour a float takes — always forgets the
    // canvas home: the operator asked to not be on the canvas.
    m_manager->setCanvasEvictor([this](const QString& containerId) {
        ContainerWidget* c = m_manager->container(containerId);
        if (c && c->isOnCanvas()) {
            evictFromCanvas(c, /*forgetHome=*/true);
        }
    });

    connect(m_manager, &ContainerManager::containerCreated,
            this, &WorkspaceController::onContainerCreated);
    for (ContainerWidget* c : m_manager->allContainers()) {
        wireContainer(c);
    }
}

// ── Identity plumbing ────────────────────────────────────────────────────
//
// Three names per applet, and the distinction matters exactly once (#1836):
// the panel entry id and the drag payload are the dragId ("TXDSP"), while
// the container's own id may differ ("tx_dsp").  Document items use the
// dragId, because that is the name the migration read out of Applet_<ID>.

ContainerWidget* WorkspaceController::containerForApplet(const QString& appletId) const
{
    if (ContainerWidget* direct = m_manager->container(appletId)) {
        return direct;
    }
    for (ContainerWidget* c : m_manager->allContainers()) {
        if (c && c->dragId() == appletId) {
            return c;
        }
    }
    return nullptr;
}

QString WorkspaceController::appletIdFor(const ContainerWidget* c) const
{
    return c ? c->dragId() : QString();
}

QString WorkspaceController::itemIdFor(const ContainerWidget* c) const
{
    return kAppletItemPrefix + appletIdFor(c);
}

// ── Lifecycle ────────────────────────────────────────────────────────────

bool WorkspaceController::boot()
{
    if (m_store.loadWithStatus() != WorkspaceStore::LoadResult::Loaded) {
        return false;
    }
    return m_store.document().canvasEnabled;
}

bool WorkspaceController::enable(const QStringList& knownAppletIds, QString* whyNot)
{
    if (m_enabled) {
        return true;
    }

    if (!m_store.isLoaded()) {
        // First enable on this install: migrate the legacy layout keys into
        // Classic.  This is the moment RFC #4887's dual-write period starts,
        // and it is deliberately here — behind the operator's explicit
        // opt-in — rather than at startup, so an install that never enables
        // the mode never gains the key.
        if (!m_store.loadOrMigrate(knownAppletIds, /*panIds=*/{})) {
            if (whyNot) *whyNot = m_store.lastError();
            return false;
        }
    }

    WorkspaceDocument doc = m_store.document();
    const Workspace* ws = doc.workspace(doc.activeWorkspace);
    if (!ws) {
        if (whyNot) *whyNot = QStringLiteral("no active workspace");
        return false;
    }
    const WorkspaceSurface* main = ws->surface(WorkspaceSurface::kMainId);
    if (!main) {
        if (whyNot) *whyNot = QStringLiteral("no main surface");
        return false;
    }

    Q_UNUSED(main);

    // Remembered for resetToClassic(), which re-derives Classic from the
    // same legacy keys the first enable migrated from.
    m_knownAppletIds = knownAppletIds;

    // The replay itself is guarded: the canvas signals it fires describe
    // what the document already says.
    m_applying = true;
    bool docChanged = false;
    placeActiveWorkspaceItems(doc, &docChanged);
    m_applying = false;

    doc.canvasEnabled = true;
    m_store.setDocument(doc);
    m_store.flush();

    m_enabled = true;
    emit enabledChanged(true);
    return true;
}

void WorkspaceController::placeActiveWorkspaceItems(WorkspaceDocument& doc,
                                                    bool* docChanged)
{
    Workspace* ws = nullptr;
    for (Workspace& w : doc.workspaces) {
        if (w.id == doc.activeWorkspace) ws = &w;
    }
    WorkspaceSurface* main = nullptr;
    if (ws) {
        for (WorkspaceSurface& surf : ws->surfaces) {
            if (surf.id == WorkspaceSurface::kMainId) main = &surf;
        }
    }
    if (!main) {
        return;
    }

    QList<CanvasItem> toPlace;
    QHash<QString, QWidget*> widgets;

    // The reserved pan area first.  If the document has never seen one
    // (first enable), it takes the region Classic left for it.
    CanvasItem panItem;
    const CanvasItem* stored = nullptr;
    for (const CanvasItem& it : main->items) {
        if (it.id == kPanStackItemId) stored = &it;
    }
    if (stored) {
        panItem = *stored;
    } else {
        panItem.id          = kPanStackItemId;
        panItem.contentType = QStringLiteral("panstack");
        panItem.rect        = panStackRectFromDocument();
        panItem.z           = -1;   // restoreItems sorts; keep it at the back
        main->items.prepend(panItem);
        if (docChanged) *docChanged = true;
    }
    panItem.minimumSize = QSize(320, 240);
    if (m_panStackWidget) {
        toPlace.append(panItem);
        widgets.insert(kPanStackItemId, m_panStackWidget.data());
    }

    for (const CanvasItem& item : main->items) {
        if (!item.id.startsWith(kAppletItemPrefix)) {
            continue;
        }
        ContainerWidget* c = containerForApplet(item.id.mid(kAppletItemPrefix.size()));
        // Closed applets keep their home but are not placed; floating ones
        // stay out (pop-out stays, RFC decision 1) until they dock.
        if (!c || !c->isContainerVisible() || c->isFloating()) {
            continue;
        }
        if (m_manager->detachForCanvas(c->id()) != c) {
            continue;
        }
        toPlace.append(item);
        widgets.insert(item.id, c);
    }

    m_canvas->restoreItems(toPlace, widgets);
}

void WorkspaceController::disable()
{
    if (!m_enabled) {
        return;
    }

    m_applying = true;
    const QStringList ids = m_canvas->layout().ids();
    for (const QString& itemId : ids) {
        if (itemId == kPanStackItemId) {
            // Released parentless; MainWindow puts it back in the splitter.
            m_canvas->takeItem(itemId);
            continue;
        }
        QWidget* w = m_canvas->takeItem(itemId);
        if (auto* c = qobject_cast<ContainerWidget*>(w)) {
            m_manager->returnFromCanvas(c->id(), c);
        }
    }
    m_applying = false;

    // Placement is kept — switching the mode off is not a statement about
    // any applet — only the flag changes.
    WorkspaceDocument doc = m_store.document();
    doc.canvasEnabled = false;
    m_store.setDocument(doc);
    m_store.flush();

    m_enabled = false;
    emit enabledChanged(false);
}

void WorkspaceController::setPanStackWidget(QWidget* w)
{
    m_panStackWidget = w;
}

// ── Applet movement ──────────────────────────────────────────────────────

bool WorkspaceController::sendAppletToCanvas(const QString& appletId,
                                             const NormRect* where)
{
    if (!m_enabled) {
        return false;
    }
    ContainerWidget* c = containerForApplet(appletId);
    if (!c || c->isOnCanvas()) {
        return false;
    }

    const QString itemId = itemIdFor(c);

    // Placement priority: the caller's rect (a drop point), else the
    // document's remembered home, else a default sized from the widget.
    NormRect rect;
    if (where) {
        rect = *where;
    } else {
        bool haveStored = false;
        const WorkspaceDocument& doc = m_store.document();
        if (const Workspace* ws = doc.workspace(doc.activeWorkspace)) {
            if (const WorkspaceSurface* main = ws->surface(WorkspaceSurface::kMainId)) {
                for (const CanvasItem& it : main->items) {
                    if (it.id == itemId) {
                        rect       = it.rect;
                        haveStored = true;
                        break;
                    }
                }
            }
        }
        if (!haveStored) {
            rect = defaultRectFor(c, nullptr);
        }
    }

    if (m_manager->detachForCanvas(c->id()) != c) {
        return false;
    }
    if (!m_canvas->addItem(itemId, c, rect, QStringLiteral("applet"))) {
        // Should not happen (id collisions are checked above), but never
        // strand a detached widget: put it straight back.
        m_manager->returnFromCanvas(c->id(), c);
        return false;
    }

    // The canvas clamped the rect; persist what is actually on screen.
    writeItemPresence(itemId, QStringLiteral("applet"),
                      m_canvas->itemRect(itemId), /*present=*/true,
                      /*flushNow=*/true);
    return true;
}

bool WorkspaceController::returnAppletToPanel(const QString& appletId)
{
    ContainerWidget* c = containerForApplet(appletId);
    if (!c || !c->isOnCanvas()) {
        return false;
    }
    evictFromCanvas(c, /*forgetHome=*/true);
    return true;
}

void WorkspaceController::evictFromCanvas(ContainerWidget* c, bool forgetHome)
{
    const QString itemId = itemIdFor(c);

    // Forget the home BEFORE the reparent: returnFromCanvas() flips the dock
    // mode to PanelDocked, and the dockModeChanged hook re-sends any open
    // applet whose item still exists — removing it first is what makes an
    // explicit return stick instead of bouncing straight back.
    if (forgetHome) {
        writeItemPresence(itemId, QStringLiteral("applet"), NormRect{},
                          /*present=*/false, /*flushNow=*/true);
    }

    m_canvas->takeItem(itemId);
    m_manager->returnFromCanvas(c->id(), c);
}

// ── Canvas events ────────────────────────────────────────────────────────

void WorkspaceController::onDropReceived(const QString& payload, const QPointF& pos)
{
    if (!m_enabled) {
        return;
    }
    ContainerWidget* c = containerForApplet(payload);
    if (!c) {
        return;
    }

    if (c->isOnCanvas()) {
        // Move: keep the size, centre the item on the drop point, and let
        // the canvas clamp.  itemRectChanged writes the document; this is a
        // discrete gesture, so flush behind it.
        const QString itemId = itemIdFor(c);
        const NormRect cur   = m_canvas->itemRect(itemId);
        NormRect moved       = cur;
        moved.x              = pos.x() - cur.w / 2.0;
        moved.y              = pos.y() - cur.h / 2.0;
        m_canvas->setItemRect(itemId, moved);
        m_store.flush();
        return;
    }

    NormRect rect = defaultRectFor(c, &pos);
    sendAppletToCanvas(appletIdFor(c), &rect);
}

void WorkspaceController::onItemRectChanged(const QString& itemId, const NormRect& rect)
{
    if (m_applying || !m_enabled) {
        return;
    }
    // Only placement gestures reach here — a canvas resize emits nothing by
    // design (stored rects are canvas-independent; the display clamp absorbs
    // small windows).  Touch, don't flush: discrete gestures flush at their
    // own call sites, and anything that streams coalesces in the debounce.
    writeItemRect(itemId, rect, /*flushNow=*/false);
}

void WorkspaceController::onContainerCreated(const QString& containerId)
{
    if (ContainerWidget* c = m_manager->container(containerId)) {
        wireContainer(c);
    }
}

void WorkspaceController::wireContainer(ContainerWidget* c)
{
    // Live move (phase 5): the title bar streams the gesture; the canvas
    // session makes the item follow the cursor with snapping.
    connect(c, &ContainerWidget::canvasDragBegan, this,
            [this, c](const QPoint& g) {
                if (m_enabled && c->isOnCanvas()) {
                    m_canvas->beginMoveGesture(itemIdFor(c), g);
                }
            });
    connect(c, &ContainerWidget::canvasDragMoved, this,
            [this, c](const QPoint& g) {
                if (m_enabled && c->isOnCanvas()) {
                    m_canvas->moveGesture(g);
                }
            });
    connect(c, &ContainerWidget::canvasDragEnded, this,
            [this, c](const QPoint& g) {
                if (m_enabled && c->isOnCanvas()) {
                    m_canvas->endGesture(g);
                }
            });

    // Open/close from the bar buttons.  Closing an applet that lives on the
    // canvas evicts it but keeps its home; reopening returns it there.
    connect(c, &ContainerWidget::visibilityChanged, this, [this, c](bool visible) {
        if (m_applying || !m_enabled) return;
        if (!visible && c->isOnCanvas()) {
            evictFromCanvas(c, /*forgetHome=*/false);
        } else if (visible && !c->isOnCanvas() && !c->isFloating()) {
            const WorkspaceDocument& doc = m_store.document();
            if (const Workspace* ws = doc.workspace(doc.activeWorkspace)) {
                if (const WorkspaceSurface* main =
                        ws->surface(WorkspaceSurface::kMainId)) {
                    for (const CanvasItem& it : main->items) {
                        if (it.id == itemIdFor(c)) {
                            sendAppletToCanvas(appletIdFor(c));
                            break;
                        }
                    }
                }
            }
        }
    });

    // A float that started from the canvas forgot its home on the way out
    // (the evictor).  The one dock that must RETURN to the canvas is the
    // applet whose item survived — floated while the mode was off, docked
    // while it is on.
    connect(c, &ContainerWidget::dockModeChanged, this,
            [this, c](ContainerWidget::DockMode mode) {
                if (m_applying || !m_enabled) return;
                if (mode != ContainerWidget::DockMode::PanelDocked) return;
                if (!c->isContainerVisible()) return;
                const WorkspaceDocument& doc = m_store.document();
                if (const Workspace* ws = doc.workspace(doc.activeWorkspace)) {
                    if (const WorkspaceSurface* main =
                            ws->surface(WorkspaceSurface::kMainId)) {
                        for (const CanvasItem& it : main->items) {
                            if (it.id == itemIdFor(c)) {
                                sendAppletToCanvas(appletIdFor(c));
                                break;
                            }
                        }
                    }
                }
            });
}

// ── Document edits ───────────────────────────────────────────────────────

void WorkspaceController::writeItemRect(const QString& itemId, const NormRect& rect,
                                        bool flushNow)
{
    WorkspaceDocument doc = m_store.document();
    for (Workspace& ws : doc.workspaces) {
        if (ws.id != doc.activeWorkspace) continue;
        for (WorkspaceSurface& s : ws.surfaces) {
            if (s.id != WorkspaceSurface::kMainId) continue;
            for (CanvasItem& it : s.items) {
                if (it.id == itemId) {
                    it.rect = rect;
                    m_store.setDocument(doc);
                    if (flushNow) m_store.flush();
                    return;
                }
            }
        }
    }
    // No item — a rect change for something the document does not track
    // (e.g. the pan stack before its first persist) is recorded by adding it.
    if (itemId == kPanStackItemId) {
        writeItemPresence(itemId, QStringLiteral("panstack"), rect,
                          /*present=*/true, flushNow);
    }
}

void WorkspaceController::writeItemPresence(const QString& itemId,
                                            const QString& contentType,
                                            const NormRect& rect, bool present,
                                            bool flushNow)
{
    WorkspaceDocument doc = m_store.document();
    for (Workspace& ws : doc.workspaces) {
        if (ws.id != doc.activeWorkspace) continue;
        for (WorkspaceSurface& s : ws.surfaces) {
            if (s.id != WorkspaceSurface::kMainId) continue;

            int found = -1;
            for (int i = 0; i < s.items.size(); ++i) {
                if (s.items.at(i).id == itemId) { found = i; break; }
            }

            if (present) {
                if (found >= 0) {
                    s.items[found].rect = rect;
                } else {
                    CanvasItem item;
                    item.id          = itemId;
                    item.contentType = contentType;
                    item.rect        = rect;
                    item.z           = m_canvas->layout().zOf(itemId);
                    s.items.append(item);
                }
            } else if (found >= 0) {
                s.items.removeAt(found);
            }

            m_store.setDocument(doc);
            if (flushNow) m_store.flush();
            return;
        }
    }
}

void WorkspaceController::writeStackingFromCanvas()
{
    WorkspaceDocument doc = m_store.document();
    for (Workspace& ws : doc.workspaces) {
        if (ws.id != doc.activeWorkspace) continue;
        for (WorkspaceSurface& s : ws.surfaces) {
            if (s.id != WorkspaceSurface::kMainId) continue;
            for (CanvasItem& it : s.items) {
                const int z = m_canvas->layout().zOf(it.id);
                if (z >= 0) it.z = z;
            }
        }
    }
    m_store.setDocument(doc);
    m_store.flush();
}

// ── Escape hatches (phase 5) ─────────────────────────────────────────────

void WorkspaceController::setReturnTarget(QWidget* target)
{
    m_returnTarget = target;
}

bool WorkspaceController::undoLastPlacement()
{
    if (!m_enabled || !canUndo()) {
        return false;
    }
    if (!m_canvas->layout().contains(m_undoItemId)) {
        m_undoItemId.clear();
        return false;
    }
    // Swap current and remembered: undoing twice toggles, which is the
    // single-slot version of redo.
    const NormRect current = m_canvas->itemRect(m_undoItemId);
    m_canvas->setItemRect(m_undoItemId, m_undoRect);
    m_undoRect = current;
    m_store.flush();
    return true;
}

void WorkspaceController::resetToClassic()
{
    if (!m_enabled) {
        return;
    }

    m_applying = true;

    // Everything off the surface: applets back to their panel slots, the
    // pan stack held aside for the re-place below.
    const QStringList ids = m_canvas->layout().ids();
    for (const QString& itemId : ids) {
        if (itemId == kPanStackItemId) {
            m_canvas->takeItem(itemId);
            continue;
        }
        QWidget* w = m_canvas->takeItem(itemId);
        if (auto* c = qobject_cast<ContainerWidget*>(w)) {
            m_manager->returnFromCanvas(c->id(), c);
        }
    }

    // Classic is re-derived from the same legacy keys the first enable
    // migrated from — the panel still dual-writes them, so this reflects
    // the operator's CURRENT open applets and order, not a stale snapshot.
    // One definition of Classic, used everywhere (WorkspaceMigration).
    WorkspaceDocument doc = m_store.document();
    const WorkspaceDocument classic =
        buildClassicDocument(readLegacyLayoutState(m_knownAppletIds), {});
    if (const Workspace* freshWs = classic.workspace(classicWorkspaceId())) {
        if (const WorkspaceSurface* freshMain =
                freshWs->surface(WorkspaceSurface::kMainId)) {
            for (Workspace& w : doc.workspaces) {
                if (w.id != doc.activeWorkspace) continue;
                for (WorkspaceSurface& surf : w.surfaces) {
                    if (surf.id == WorkspaceSurface::kMainId) {
                        surf.items = freshMain->items;
                    }
                }
            }
        }
    }

    placeActiveWorkspaceItems(doc, nullptr);
    m_applying = false;

    m_undoItemId.clear();   // a whole-surface change; a one-rect undo would lie
    m_store.setDocument(doc);
    m_store.flush();
}

void WorkspaceController::tidyLayout()
{
    if (!m_enabled) {
        return;
    }

    QList<CanvasItem> items;
    for (const CanvasItem& it : m_canvas->layout().itemsByZ()) {
        items.append(it);
    }
    const QList<TidyMove> moves =
        tidyOverlaps(items, QStringList{kPanStackItemId});

    for (const TidyMove& mv : moves) {
        m_canvas->setItemRect(mv.id, mv.rect);   // rect stream → touch
    }
    if (!moves.isEmpty()) {
        m_undoItemId.clear();   // multi-item change; single-slot undo is out
        m_store.flush();
    }
}

void WorkspaceController::onItemDraggedOut(const QString& itemId,
                                           const QPoint& globalPos)
{
    if (m_applying || !m_enabled || itemId == kPanStackItemId) {
        return;
    }
    // Only a release over the return target means anything; the canvas has
    // already restored the item's rect, so anything else is a completed
    // abort.
    QWidget* target = m_returnTarget.data();
    if (!target || !target->isVisible()) {
        return;
    }
    if (!target->rect().contains(target->mapFromGlobal(globalPos))) {
        return;
    }
    if (itemId.startsWith(kAppletItemPrefix)) {
        returnAppletToPanel(itemId.mid(kAppletItemPrefix.size()));
    }
}

void WorkspaceController::onContextMenuRequested(const QString& itemId,
                                                 const QPoint& globalPos)
{
    if (!m_enabled) {
        return;
    }

    QMenu menu;
    const bool onApplet =
        !itemId.isEmpty() && itemId.startsWith(kAppletItemPrefix);

    if (onApplet) {
        const QString appletId = itemId.mid(kAppletItemPrefix.size());
        menu.addAction(QStringLiteral("Return to panel"), this,
                       [this, appletId] { returnAppletToPanel(appletId); });
        menu.addAction(QStringLiteral("Bring to front"), this,
                       [this, itemId] { m_canvas->bringItemToFront(itemId); });
        menu.addAction(QStringLiteral("Send to back"), this,
                       [this, itemId] { m_canvas->sendItemToBack(itemId); });
        menu.addSeparator();
    }

    QAction* undo = menu.addAction(QStringLiteral("Undo last placement"), this,
                                   [this] { undoLastPlacement(); });
    undo->setEnabled(canUndo());
    menu.addAction(QStringLiteral("Tidy layout"), this,
                   [this] { tidyLayout(); });
    menu.addAction(QStringLiteral("Reset layout to Classic"), this,
                   [this] { resetToClassic(); });

    menu.exec(globalPos);
}

// ── Geometry helpers ─────────────────────────────────────────────────────

NormRect WorkspaceController::defaultRectFor(const ContainerWidget* c,
                                             const QPointF* center) const
{
    // Size from the widget's own hint, normalized against the canvas — so a
    // dropped applet arrives at roughly its panel size instead of a house
    // number.  The canvas clamps against its minimum floor either way.
    const QSize canvasSize = m_canvas->size();
    const QSize hint = c ? c->sizeHint() : QSize();

    NormRect r;
    r.w = (canvasSize.width() > 0 && hint.width() > 0)
              ? qMin(0.9, hint.width() / double(canvasSize.width()))
              : 0.2;
    r.h = (canvasSize.height() > 0 && hint.height() > 0)
              ? qMin(0.9, hint.height() / double(canvasSize.height()))
              : 0.3;
    if (center) {
        r.x = center->x() - r.w / 2.0;
        r.y = center->y() - r.h / 2.0;
    } else {
        r.x = 1.0 - r.w;   // default lands where the panel column was
        r.y = 0.0;
    }
    return r;
}

NormRect WorkspaceController::panStackRectFromDocument() const
{
    // First enable: the pan area takes whatever Classic left for it — the
    // complement of the applet column, on whichever side the applets are.
    const WorkspaceDocument& doc = m_store.document();
    const Workspace* ws = doc.workspace(doc.activeWorkspace);
    const WorkspaceSurface* main = ws ? ws->surface(WorkspaceSurface::kMainId) : nullptr;

    bool haveApplets = false;
    double minX = 1.0;
    if (main) {
        for (const CanvasItem& it : main->items) {
            if (it.id.startsWith(kAppletItemPrefix)) {
                haveApplets = true;
                minX = qMin(minX, it.rect.x);
            }
        }
    }

    NormRect r{0.0, 0.0, 1.0, 1.0};
    if (haveApplets) {
        r.w = 1.0 - kClassicAppletColumnWidth;
        r.x = (minX < 0.5) ? kClassicAppletColumnWidth : 0.0;   // column left → pans right
    }
    return r;
}

}  // namespace AetherSDR
