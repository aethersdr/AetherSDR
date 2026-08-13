#include "gui/workspace/WorkspaceController.h"

#include "gui/containers/ContainerManager.h"
#include "gui/containers/ContainerWidget.h"
#include "gui/workspace/CanvasInteraction.h"
#include "gui/workspace/ClassicLayout.h"
#include "gui/workspace/WorkspaceMigration.h"
#include "gui/workspace/WorkspaceCanvas.h"

#include "core/ThemeManager.h"

#include <QHash>
#include <QMenu>
#include <QToolTip>

#include <algorithm>
#include <QPoint>
#include <QWidget>

namespace AetherSDR {

namespace {

// Menu text is MNEMONIC text: a bare '&' marks an accelerator and renders
// as an underline (or eats the character entirely) — "Audio & DSP" showed
// as "Audio _DSP" (8600 field report).  Everything data-driven that enters
// a menu goes through here.
QString menuText(const QString& s)
{
    QString t = s;
    t.replace(QLatin1Char('&'), QStringLiteral("&&"));
    return t;
}

const QString kAppletItemPrefix = QStringLiteral("applet:");

// The MIME type the panel's title-bar drags already carry (#3057); the
// canvas accepts the same one so place/move/return are all one mechanism.
const QByteArray kAppletMime = QByteArrayLiteral("application/x-aethersdr-applet");

}  // namespace

namespace {
const QString kPanItemPrefix = QStringLiteral("pan:");
}

const QString WorkspaceController::kPanStackItemId = QStringLiteral("panstack");
const QString WorkspaceController::kBandStackItemId = QStringLiteral("bandstack");

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
    bool justMigrated = false;

    if (!m_store.isLoaded()) {
        // First enable on this install: migrate the legacy layout keys into
        // Classic.  This is the moment RFC #4887's dual-write period starts,
        // and it is deliberately here — behind the operator's explicit
        // opt-in — rather than at startup, so an install that never enables
        // the mode never gains the key.
        // Remembered before the migration read so migrationPanSlotIds() can
        // consult the same legacy keys.
        m_knownAppletIds = knownAppletIds;
        if (!m_store.loadOrMigrate(knownAppletIds, migrationPanSlotIds(),
                                   &justMigrated)) {
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

    // Remembered for resetToClassic(), which re-derives Classic from the
    // same legacy keys the first enable migrated from.
    m_knownAppletIds = knownAppletIds;

    // The replay itself is guarded: the canvas signals it fires describe
    // what the document already says.
    m_applying = true;
    placeActiveWorkspaceItems(doc, nullptr);
    m_applying = false;

    doc.canvasEnabled = true;
    m_store.setDocument(doc);
    m_store.flush();

    m_enabled = true;

    // The operating posture: enabled means USING the station, so the canvas
    // comes up locked — except on the very first enable, which ran the
    // migration: the operator just opted in precisely to arrange things,
    // and greeting them with a locked surface would bury the feature they
    // asked for behind a second menu trip.  Session-transient thereafter.
    // (A local, deliberately — a member here went stale across a failed
    // enable and opened editing on a later one that was not first: m8.)
    m_canvas->setEditMode(justMigrated);

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

    // ── One-time split: the phase-3 reserved panstack item becomes per-pan
    // slot items (#4887 phase 4).  Deterministic even with zero live pans:
    // the slot count comes from the operator's own saved pan layout, and
    // the cells are carved from the panstack rect they replace, so the
    // arrangement the operator had is exactly what the slots reproduce.  A
    // phase-3 build reading the result simply recreates a full-region
    // panstack item and ignores the slots — downgrade-safe both ways.
    for (int i = 0; i < main->items.size(); ++i) {
        if (main->items.at(i).id != kPanStackItemId) continue;
        const CanvasItem panstack = main->items.takeAt(i);
        const LegacyLayoutState legacy =
            readLegacyLayoutState(m_knownAppletIds);
        QList<NormRect> cells = panCellsForLayout(legacy.panLayoutId);
        if (cells.isEmpty()) {
            cells = panCellsForLayout(QStringLiteral("1"));
        }
        int inserted = 0;
        for (int c = 0; c < cells.size(); ++c) {
            const QString slotId = kPanItemPrefix + QString::number(c);
            bool exists = false;
            for (const CanvasItem& it : main->items) {
                if (it.id == slotId) { exists = true; break; }
            }
            if (exists) continue;
            CanvasItem it;
            it.id          = slotId;
            it.contentType = QStringLiteral("panadapter");
            it.rect.x = panstack.rect.x + cells.at(c).x * panstack.rect.w;
            it.rect.y = panstack.rect.y + cells.at(c).y * panstack.rect.h;
            it.rect.w = cells.at(c).w * panstack.rect.w;
            it.rect.h = cells.at(c).h * panstack.rect.h;
            it.z = panstack.z;
            main->items.insert(i + inserted, it);
            ++inserted;
        }
        if (docChanged) *docChanged = true;
        break;
    }

    // ── Normalize: pans BELOW everything else, always ────────────────────
    //
    // The document's z is otherwise replayed verbatim, and a document that
    // lived through the frontmost-arrival bug (or any future mishap) would
    // resurrect pans over the operator's controls at every boot — the 8600
    // field report's second act.  Pans are the surface the station sits on;
    // enforcing that at replay costs a deliberate pan-over-applet stacking
    // across restarts (nothing supports one today — phase 6's pinning is
    // where that would live) and buys layouts that cannot rot.
    {
        QList<CanvasItem> ordered = main->items;
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const CanvasItem& a, const CanvasItem& b) {
                             return a.z < b.z;
                         });
        std::stable_partition(ordered.begin(), ordered.end(),
                              [](const CanvasItem& it) {
                                  return it.id.startsWith(kPanItemPrefix);
                              });
        bool zChanged = false;
        for (int i = 0; i < ordered.size(); ++i) {
            if (ordered[i].z != i) {
                ordered[i].z = i;
                zChanged = true;
            }
        }
        main->items = ordered;
        if (zChanged && docChanged) *docChanged = true;
    }

    // ── Pans, from their slot items ──────────────────────────────────────
    const QStringList livePans =
        m_panHost.panIds ? m_panHost.panIds() : QStringList{};
    for (const QString& panId : livePans) {
        if (m_panHost.isFloating && m_panHost.isFloating(panId)) {
            continue;   // pop-out stays (RFC decision 1)
        }
        const QString itemId = panItemIdFor(panId);
        CanvasItem item;
        bool found = false;
        for (const CanvasItem& it : main->items) {
            if (it.id == itemId) { item = it; found = true; break; }
        }
        if (!found) {
            item.id          = itemId;
            item.contentType = QStringLiteral("panadapter");
            // Cascade, exactly like sendPanToCanvas() (review: a Blank
            // workspace placed every live pan at one identical rect —
            // "cascade defaults" was true on the arrival path only).
            const int slot = slotForPan(panId);
            const double step = 0.05 * (slot % 5);
            item.rect        = NormRect{0.15 + step, 0.15 + step, 0.6, 0.6};
            item.z           = 0;
            main->items.append(item);
            if (docChanged) *docChanged = true;
        }
        item.minimumSize = QSize(320, 180);
        QWidget* w = m_panHost.detach ? m_panHost.detach(panId) : nullptr;
        if (!w) continue;
        toPlace.append(item);
        widgets.insert(itemId, w);
    }

    for (const CanvasItem& item : main->items) {
        if (!item.id.startsWith(kAppletItemPrefix)) {
            continue;
        }
        ContainerWidget* c = containerForApplet(item.id.mid(kAppletItemPrefix.size()));
        // A closed-flagged item belongs to the workspace but its applet is
        // shut (phase 6 full recall) — placement skips it.  UNLESS the
        // applet is currently OPEN: it was reopened while the mode was off
        // (the visibility hook early-returns there), and the operator's
        // click outranks the stale flag (review, K6OZY) — clear and place.
        if (item.closed) {
            if (!c || !c->isContainerVisible() || c->isFloating()) {
                continue;
            }
            for (CanvasItem& live : main->items) {
                if (live.id == item.id) {
                    live.closed = false;
                    if (docChanged) *docChanged = true;
                    break;
                }
            }
        }
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
    releaseAllItems(/*returnPansToStack=*/true, /*hideBandStack=*/false);
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

void WorkspaceController::setPanHost(const PanHostHooks& hooks)
{
    m_panHost = hooks;
}

// ── Pans as items (RFC #4887 phase 4) ────────────────────────────────────

QStringList WorkspaceController::effectiveKnownAppletIds() const
{
    if (!m_knownAppletIds.isEmpty()) {
        return m_knownAppletIds;
    }
    QStringList ids;
    for (const WidgetCatalogEntry& e : m_widgetCatalog) {
        ids.append(e.id);
    }
    return ids;
}

int WorkspaceController::slotForPan(const QString& panId)
{
    const auto it = m_panSlots.constFind(panId);
    if (it != m_panSlots.constEnd()) {
        return it.value();
    }
    // Lowest free, so remove-then-add reuses the slot — the same discipline
    // as RadioModel::neutralPanIndexFor, for the same reason.
    int slot = 0;
    const QList<int> used = m_panSlots.values();
    while (used.contains(slot)) {
        ++slot;
    }
    m_panSlots.insert(panId, slot);
    return slot;
}

QString WorkspaceController::panItemIdFor(const QString& panId)
{
    return kPanItemPrefix + QString::number(slotForPan(panId));
}

QString WorkspaceController::panIdForItem(const QString& itemId) const
{
    if (!itemId.startsWith(kPanItemPrefix)) {
        return QString();
    }
    bool ok = false;
    const int slot = itemId.mid(kPanItemPrefix.size()).toInt(&ok);
    if (!ok) {
        return QString();
    }
    for (auto it = m_panSlots.constBegin(); it != m_panSlots.constEnd(); ++it) {
        if (it.value() == slot) {
            return it.key();
        }
    }
    return QString();
}

QStringList WorkspaceController::migrationPanSlotIds() const
{
    const int live = m_panHost.panIds
                         ? static_cast<int>(m_panHost.panIds().size())
                         : 0;
    const LegacyLayoutState legacy = readLegacyLayoutState(m_knownAppletIds);
    const int fromLayout = panCountForLayout(legacy.panLayoutId);
    const int count = qMax(1, qMax(live, fromLayout));
    QStringList ids;
    for (int i = 0; i < count; ++i) {
        ids.append(QString::number(i));
    }
    return ids;
}

bool WorkspaceController::sendPanToCanvas(const QString& panId)
{
    if (!m_enabled) {
        return false;
    }
    if (m_panHost.isFloating && m_panHost.isFloating(panId)) {
        return false;   // pop-out stays (RFC decision 1)
    }
    const QString itemId = panItemIdFor(panId);
    if (m_canvas->contains(itemId)) {
        QWidget* placed = m_canvas->itemWidget(itemId);
        if (placed && placed->parentWidget() == m_canvas) {
            return true;   // genuinely placed — the state the caller asked for
        }
        // The entry is a lie: the widget was reclaimed behind the canvas's
        // back (a stack rebuild that predates the loan set, or any future
        // path that forgets it).  Heal instead of trusting: drop the stale
        // entry and fall through to a fresh detach-and-place.
        m_canvas->releaseItem(itemId);
    }

    bool haveStored = false;
    NormRect rect{0.2, 0.2, 0.6, 0.6};
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
        // Cascade, not one shared rect (red-team B3): every defaulted pan
        // landing at the same {0.2,0.2,0.6,0.6} made `pan create` a visual
        // no-op — three new pans stacked pixel-identical behind the first.
        // The classic window-manager stagger, keyed by slot so it is
        // deterministic; clamped by addItem either way.
        const int slot = slotForPan(panId);
        const double step = 0.05 * (slot % 5);
        rect.x = 0.15 + step;
        rect.y = 0.15 + step;
    }

    QWidget* w = m_panHost.detach ? m_panHost.detach(panId) : nullptr;
    if (!w) {
        return false;
    }
    if (!m_canvas->addItem(itemId, w, rect, QStringLiteral("panadapter"),
                           QSize(320, 180))) {
        if (m_panHost.restore) {
            m_panHost.restore(panId, w);   // never strand a detached applet
        }
        return false;
    }
    // Below every applet, always — pans are the SURFACE the station sits
    // on ("a meter over the spectrum is a feature"), and a pan landing
    // frontmost swallowed the operator's clicks and wheel (the 8600 field
    // report: only the pan and its VFO flag still responded).  A stored
    // rect (arrival into an existing arrangement) goes to the very back;
    // a DEFAULTED rect is a deliberate new pan, and burying it under the
    // older pans made `pan create` invisible (red-team B3, maintainer
    // ruling): it stacks ABOVE the other pans, still below all applets.
    m_canvas->sendItemToBack(itemId);
    if (!haveStored) {
        int otherPans = 0;
        for (const CanvasItem& it : m_canvas->layout().itemsByZ()) {
            if (it.id != itemId && it.id.startsWith(kPanItemPrefix)) {
                ++otherPans;
            }
        }
        for (int i = 0; i < otherPans; ++i) {
            m_canvas->raiseItem(itemId);
        }
    }
    writeItemPresence(itemId, QStringLiteral("panadapter"),
                      m_canvas->itemRect(itemId), /*present=*/true,
                      /*flushNow=*/true);
    return true;
}

void WorkspaceController::onPanAdded(const QString& panId)
{
    if (!m_enabled) {
        return;
    }
    sendPanToCanvas(panId);
}

void WorkspaceController::onPanRemoved(const QString& panId)
{
    // The stack emits this BEFORE destroying the applet, so releasing here
    // (never reparenting — the widget is about to die) beats waiting for
    // the destroyed-watch.  The document item is KEPT: a pan closing is not
    // a statement about its spot, and the next pan in this slot takes it.
    const auto it = m_panSlots.constFind(panId);
    if (it != m_panSlots.constEnd()) {
        m_canvas->releaseItem(kPanItemPrefix + QString::number(it.value()));
        m_panSlots.erase(it);
    }
}

void WorkspaceController::onPanRekeyed(const QString& oldId, const QString& newId)
{
    // Same applet, new radio id (FLEX band recall): the slot — and with it
    // the canvas item — follows the applet.
    const auto it = m_panSlots.constFind(oldId);
    if (it != m_panSlots.constEnd()) {
        const int slot = it.value();
        m_panSlots.erase(it);
        m_panSlots.insert(newId, slot);
    }
}

void WorkspaceController::onPanFloated(const QString& panId)
{
    // The applet has already been adopted by its PanFloatingWindow — release
    // the entry, never take it (#1344).  Slot and document item survive:
    // a pan has no panel to be returned to, so its canvas home is where
    // docking brings it back (unlike applets, floating forgets nothing).
    const auto it = m_panSlots.constFind(panId);
    if (it != m_panSlots.constEnd()) {
        m_canvas->releaseItem(kPanItemPrefix + QString::number(it.value()));
    }
}

void WorkspaceController::onPanDocked(const QString& panId)
{
    if (!m_enabled) {
        return;
    }
    // dockPanadapter() has just re-homed the applet into the (hidden) stack
    // splitter; bring it back to its canvas spot.
    sendPanToCanvas(panId);
}

void WorkspaceController::beginPanItemMove(const QString& panId, const QPoint& globalPos)
{
    if (!m_enabled) {
        return;
    }
    const QString itemId = panItemIdFor(panId);
    if (m_canvas->contains(itemId)) {
        m_canvas->beginMoveGesture(itemId, globalPos);
    }
}

void WorkspaceController::movePanItem(const QPoint& globalPos)
{
    if (m_enabled) {
        m_canvas->moveGesture(globalPos);
    }
}

void WorkspaceController::endPanItemMove(const QPoint& globalPos)
{
    if (m_enabled) {
        m_canvas->endGesture(globalPos);
    }
}

void WorkspaceController::setBandStackVisible(bool on)
{
    if (!m_enabled || !m_panHost.bandStack) {
        return;
    }
    QWidget* panel = m_panHost.bandStack();
    if (!panel) {
        return;
    }

    if (!on) {
        if (m_canvas->contains(kBandStackItemId)) {
            QWidget* w = m_canvas->releaseItem(kBandStackItemId);
            if (w && m_panHost.reclaimBandStack) {
                m_panHost.reclaimBandStack(w);
            }
            if (w) {
                w->hide();
            }
        }
        return;
    }
    if (m_canvas->contains(kBandStackItemId)) {
        return;
    }

    // Its remembered spot, else a strip down the left edge.
    NormRect rect{0.0, 0.0, 0.07, 1.0};
    const WorkspaceDocument& doc = m_store.document();
    if (const Workspace* ws = doc.workspace(doc.activeWorkspace)) {
        if (const WorkspaceSurface* main = ws->surface(WorkspaceSurface::kMainId)) {
            for (const CanvasItem& it : main->items) {
                if (it.id == kBandStackItemId) {
                    rect = it.rect;
                    break;
                }
            }
        }
    }
    if (m_canvas->addItem(kBandStackItemId, panel, rect,
                          QStringLiteral("bandstack"), QSize(90, 240))) {
        panel->show();
        writeItemPresence(kBandStackItemId, QStringLiteral("bandstack"),
                          m_canvas->itemRect(kBandStackItemId),
                          /*present=*/true, /*flushNow=*/true);
    }
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
            // Full recall (phase 6): the workspace remembers this applet as
            // closed, so switching back does not resurrect it.
            writeItemClosed(itemIdFor(c), true, /*flushNow=*/true);
        } else if (!visible && c->isFloating()) {
            // A pop-out closed from its float window deserts silently
            // otherwise (review m8): the workspace still listed it open —
            // neither open nor placed, recoverable only via the palette.
            writeItemClosed(itemIdFor(c), true, /*flushNow=*/true);
        } else if (visible && !c->isOnCanvas() && !c->isFloating()) {
            const WorkspaceDocument& doc = m_store.document();
            if (const Workspace* ws = doc.workspace(doc.activeWorkspace)) {
                if (const WorkspaceSurface* main =
                        ws->surface(WorkspaceSurface::kMainId)) {
                    for (const CanvasItem& it : main->items) {
                        if (it.id == itemIdFor(c)) {
                            // sendAppletToCanvas's writeItemPresence clears
                            // the closed flag — the invariant lives there.
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
    // No item — a rect change for something the document does not track is
    // recorded by adding it (a pan placed before its slot item existed, the
    // band stack, or a phase-3 panstack).
    if (itemId.startsWith(kPanItemPrefix)) {
        writeItemPresence(itemId, QStringLiteral("panadapter"), rect,
                          /*present=*/true, flushNow);
    } else if (itemId == kBandStackItemId) {
        writeItemPresence(itemId, QStringLiteral("bandstack"), rect,
                          /*present=*/true, flushNow);
    } else if (itemId == kPanStackItemId) {
        writeItemPresence(itemId, QStringLiteral("panstack"), rect,
                          /*present=*/true, flushNow);
    }
}

void WorkspaceController::writeItemClosed(const QString& itemId, bool closed,
                                          bool flushNow)
{
    WorkspaceDocument doc = m_store.document();
    for (Workspace& ws : doc.workspaces) {
        if (ws.id != doc.activeWorkspace) continue;
        for (WorkspaceSurface& s : ws.surfaces) {
            if (s.id != WorkspaceSurface::kMainId) continue;
            for (CanvasItem& it : s.items) {
                if (it.id == itemId && it.closed != closed) {
                    it.closed = closed;
                    m_store.setDocument(doc);
                    if (flushNow) m_store.flush();
                    return;
                }
            }
        }
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
                    // Making an item PRESENT is the one statement that its
                    // applet is wanted on the surface — the closed flag
                    // clears here, in one place, instead of at whichever
                    // call sites remembered to (review, K6OZY: two of four
                    // compensated by hand and the reopen-while-disabled
                    // path cleared nothing).
                    s.items[found].closed = false;
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

    // Everything off the surface: applets back to their panel slots, pans
    // merely RELEASED (they stay parented to the canvas; the re-place below
    // re-adds them — no nullptr detour, #1344), the band stack re-homed and
    // hidden (Classic is the stock shell, and the stock shell shows none).
    const QStringList ids = m_canvas->layout().ids();
    for (const QString& itemId : ids) {
        if (itemId.startsWith(kPanItemPrefix)) {
            m_canvas->releaseItem(itemId);
            continue;
        }
        if (itemId == kBandStackItemId) {
            QWidget* w = m_canvas->releaseItem(itemId);
            if (w && m_panHost.reclaimBandStack) {
                m_panHost.reclaimBandStack(w);
            }
            if (w) w->hide();
            continue;
        }
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
    // Classic is composed against the pans' ACTUAL slots — sparse and all
    // (review, K6OZY): the old global renumber fixed the active workspace
    // and orphaned every OTHER workspace's pan items, since slots are the
    // cross-workspace identity.  Sparse slots just mean sparse Classic
    // cells; nothing renumbers.
    QStringList slotIds;
    {
        QList<int> slotVals = m_panSlots.values();
        std::sort(slotVals.begin(), slotVals.end());
        for (int v : slotVals) slotIds.append(QString::number(v));
        if (slotIds.isEmpty()) slotIds = migrationPanSlotIds();
    }
    const WorkspaceDocument classic =
        buildClassicDocument(readLegacyLayoutState(effectiveKnownAppletIds()),
                             slotIds);
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
    QStringList fixedIds{kPanStackItemId};
    for (const CanvasItem& it : m_canvas->layout().itemsByZ()) {
        items.append(it);
        // Every pan item is fixed: an applet overlapping the spectrum is a
        // feature (the phase-5 rule), and tidy shoving the spectrum itself
        // around would be the disorder it exists to fix.
        if (it.id.startsWith(kPanItemPrefix)) {
            fixedIds.append(it.id);
        }
    }
    const QList<TidyMove> moves = tidyOverlaps(items, fixedIds);

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
    if (m_applying || !m_enabled || itemId == kPanStackItemId
        || itemId == kBandStackItemId || itemId.startsWith(kPanItemPrefix)) {
        return;   // only applets have a panel to land on
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

    // Edit Layout leads in BOTH postures — it is the door between them —
    // and everything below it is placement, so a locked canvas shows only
    // the door.
    QAction* editToggle = menu.addAction(QStringLiteral("Edit layout"));
    editToggle->setCheckable(true);
    editToggle->setChecked(m_canvas->isEditMode());
    connect(editToggle, &QAction::toggled, this,
            [this](bool on) { m_canvas->setEditMode(on); });

    // The workspace switcher rides in BOTH postures (phase 6): switching is
    // operating, not editing.
    const QList<QPair<QString, QString>> wsList = workspaceList();
    if (wsList.size() > 1) {
        QMenu* switcher = menu.addMenu(QStringLiteral("Workspace"));
        const QString active = activeWorkspaceId();
        for (const auto& [wsId, wsLabel] : wsList) {
            QAction* a = switcher->addAction(menuText(wsLabel));
            a->setCheckable(true);
            a->setChecked(wsId == active);
            connect(a, &QAction::triggered, this,
                    [this, wsId] { switchWorkspace(wsId); });
        }
    }

    // The palette rides in BOTH postures (review m3): with the panel gone,
    // it is the only mouse path to OPEN an applet, and opening a tool is
    // operating.  Placement of what already exists stays edit-only.
    menu.addSeparator();
    // Add widget ▸ (field request): every applet by functional category.
    // Ones already on the canvas render as a checked, disabled entry with a
    // highlight bar — visibly present, not re-addable.
    if (!m_widgetCatalog.isEmpty()) {
        QMenu* add = menu.addMenu(QStringLiteral("Add widget"));
        theme::setContainer(add, QStringLiteral("root"));
        ThemeManager::instance().applyStyleSheet(add,
            QStringLiteral("QMenu::item:disabled:checked {"
                           " background: {{color.accent.dim}};"
                           " color: {{color.background.0}}; }"));
        const QPointF canvasPos =
            m_canvas->rect().isEmpty()
                ? QPointF(0.5, 0.5)
                : QPointF(m_canvas->mapFromGlobal(globalPos).x()
                              / double(m_canvas->width()),
                          m_canvas->mapFromGlobal(globalPos).y()
                              / double(m_canvas->height()));

        const QList<PaletteEntry> pal = paletteState();
        QStringList categoryOrder;
        for (const PaletteEntry& e : pal) {
            if (!categoryOrder.contains(e.category)) {
                categoryOrder.append(e.category);
            }
        }
        for (const QString& category : categoryOrder) {
            QMenu* catMenu = add->addMenu(menuText(category));
            for (const PaletteEntry& e : pal) {
                if (e.category != category) {
                    continue;
                }
                switch (e.state) {
                case PaletteEntry::State::OnCanvas: {
                    QAction* a = catMenu->addAction(menuText(e.title));
                    a->setCheckable(true);
                    a->setChecked(true);
                    a->setEnabled(false);
                    break;
                }
                case PaletteEntry::State::NotDetected: {
                    // GREY, never hidden — the category and taxonomy stay
                    // visible (the 8600 amplifier regression) and the
                    // entry recovers on the next menu open once detection
                    // flips.  The suffix says WHY it is dimmed: a screen
                    // reader hears "disabled" and nothing else, and this
                    // grey means three different things (#4968 m3, a11y
                    // commitment in #4896).
                    QAction* a = catMenu->addAction(
                        menuText(e.title)
                        + tr(" (not detected)"));
                    a->setEnabled(false);
                    break;
                }
                case PaletteEntry::State::Absent: {
                    // Catalogued but not constructed in this session — a
                    // live-looking entry that silently no-ops is worse
                    // than a grey one (review M3).
                    QAction* a = catMenu->addAction(menuText(e.title));
                    a->setEnabled(false);
                    break;
                }
                case PaletteEntry::State::Addable: {
                    QAction* a = catMenu->addAction(menuText(e.title));
                    const QString appletId = e.id;
                    const QString title    = e.title;
                    connect(a, &QAction::triggered, this,
                            [this, appletId, title, canvasPos, globalPos] {
                                if (!addAppletFromPalette(appletId,
                                                          canvasPos)) {
                                    // Detection flipped between menu build
                                    // and click (#4968 m1) — a refused add
                                    // must never be a silent no-op.
                                    QToolTip::showText(
                                        globalPos,
                                        tr("%1 is not available right now")
                                            .arg(title));
                                }
                            });
                    break;
                }
                }
            }
        }
        menu.addSeparator();
    }


    if (!m_canvas->isEditMode()) {
        menu.exec(globalPos);
        return;
    }
    menu.addSeparator();

    const bool onApplet =
        !itemId.isEmpty() && itemId.startsWith(kAppletItemPrefix);
    const bool onPan = !itemId.isEmpty() && itemId.startsWith(kPanItemPrefix);

    if (onPan) {
        const QString panId = panIdForItem(itemId);
        QAction* popOut = menu.addAction(QStringLiteral("Pop out"), this,
                                         [this, panId] {
                                             if (m_panHost.requestFloat) {
                                                 m_panHost.requestFloat(panId);
                                             }
                                         });
        popOut->setEnabled(!panId.isEmpty()
                           && static_cast<bool>(m_panHost.requestFloat));
        // No "Bring to front" for pans (review m3, maintainer ruling): the
        // replay normalization exists to keep pans below the controls and
        // would undo it at the next mode cycle — offering a control the
        // persistence layer reverts is worse than not offering it.
        // Deliberate pan-over-applet layering arrives with phase 6's
        // pinning, as a property the normalization respects.
        menu.addAction(QStringLiteral("Send to back"), this,
                       [this, itemId] { m_canvas->sendItemToBack(itemId); });
        menu.addSeparator();
    }
    if (itemId == kBandStackItemId) {
        menu.addAction(QStringLiteral("Hide band stack"), this,
                       [this] { setBandStackVisible(false); });
        menu.addSeparator();
    }

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

    QAction* gridSnap = menu.addAction(QStringLiteral("Snap to grid"));
    gridSnap->setCheckable(true);
    gridSnap->setChecked(m_canvas->isGridSnapEnabled());
    connect(gridSnap, &QAction::toggled, this,
            [this](bool on) { m_canvas->setGridSnapEnabled(on); });

    QAction* undo = menu.addAction(QStringLiteral("Undo last placement"), this,
                                   [this] { undoLastPlacement(); });
    undo->setEnabled(canUndo());
    menu.addAction(QStringLiteral("Tidy layout"), this,
                   [this] { tidyLayout(); });
    menu.addAction(QStringLiteral("Reset layout to Classic"), this,
                   [this] { resetToClassic(); });

    menu.exec(globalPos);
}

void WorkspaceController::releaseAllItems(bool returnPansToStack,
                                          bool hideBandStack)
{
    const QStringList ids = m_canvas->layout().ids();
    for (const QString& itemId : ids) {
        if (itemId.startsWith(kPanItemPrefix)) {
            // Release, never take: the restore hook reparents in ONE step
            // (addWidget), and the nullptr detour is forbidden for QRhi
            // children (#1344).  On a workspace SWITCH the pan stays
            // parented to the canvas — the placement that follows re-adds
            // it, a canvas→canvas move.
            QWidget* w = m_canvas->releaseItem(itemId);
            const QString panId = panIdForItem(itemId);
            if (returnPansToStack && w && !panId.isEmpty() && m_panHost.restore) {
                m_panHost.restore(panId, w);
            }
            continue;
        }
        if (itemId == kBandStackItemId) {
            QWidget* w = m_canvas->releaseItem(itemId);
            if (w && m_panHost.reclaimBandStack) {
                m_panHost.reclaimBandStack(w);
            }
            if (w && hideBandStack) {
                w->hide();   // session-transient; a switch starts it hidden
            }
            continue;
        }
        if (itemId == kPanStackItemId) {
            // Phase-3 document robustness: released parentless; MainWindow
            // puts it back in the splitter.
            m_canvas->takeItem(itemId);
            continue;
        }
        QWidget* w = m_canvas->takeItem(itemId);
        if (auto* c = qobject_cast<ContainerWidget*>(w)) {
            m_manager->returnFromCanvas(c->id(), c);
        }
    }
}

// ── Workspaces (RFC #4887 phase 6) ───────────────────────────────────────

QString WorkspaceController::activeWorkspaceId() const
{
    return m_store.document().activeWorkspace;
}

QList<QPair<QString, QString>> WorkspaceController::workspaceList() const
{
    QList<QPair<QString, QString>> out;
    const WorkspaceDocument& doc = m_store.document();
    for (const Workspace& w : doc.workspaces) {
        out.append({w.id, w.label.isEmpty() ? w.id : w.label});
    }
    return out;
}

QString WorkspaceController::createWorkspace(NewWorkspaceSource source,
                                             const QString& label)
{
    if (!m_store.isLoaded()) {
        return QString();
    }
    WorkspaceDocument doc = m_store.document();

    QString newId;
    switch (source) {
    case NewWorkspaceSource::Current:
        newId = doc.addDuplicateOf(doc.activeWorkspace, label);
        break;
    case NewWorkspaceSource::Blank:
        newId = doc.addBlank(label);
        break;
    case NewWorkspaceSource::Classic: {
        newId = doc.addBlank(label);
        if (!newId.isEmpty()) {
            // One definition of Classic, used everywhere: composed from the
            // live legacy keys, exactly as resetToClassic() does.  The
            // applet universe falls back to the widget catalog when the
            // canvas has not been enabled this session (review, K6OZY:
            // m_knownAppletIds is only assigned in enable(), so
            // loaded-but-disabled composed Classic from NOTHING).
            const WorkspaceDocument classic = buildClassicDocument(
                readLegacyLayoutState(effectiveKnownAppletIds()),
                migrationPanSlotIds());
            if (const Workspace* freshWs =
                    classic.workspace(classicWorkspaceId())) {
                if (const WorkspaceSurface* freshMain =
                        freshWs->surface(WorkspaceSurface::kMainId)) {
                    for (Workspace& w : doc.workspaces) {
                        if (w.id != newId) continue;
                        for (WorkspaceSurface& surf : w.surfaces) {
                            if (surf.id == WorkspaceSurface::kMainId) {
                                surf.items = freshMain->items;
                            }
                        }
                    }
                }
            }
        }
        break;
    }
    }
    if (newId.isEmpty()) {
        return QString();
    }
    m_store.setDocument(doc);
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    emit workspacesChanged();

    // Creating is arranging: while the mode is on, the new workspace
    // becomes the active one immediately — and opens EDITING (field
    // request): the operator just named a canvas to lay out, and greeting
    // them with a locked surface would send them straight back to the menu.
    // The same reasoning as the first-enable-after-migration rule.
    if (m_enabled) {
        switchWorkspace(newId);
        m_canvas->setEditMode(true);
    }
    return newId;
}

bool WorkspaceController::renameWorkspace(const QString& id, const QString& label)
{
    WorkspaceDocument doc = m_store.document();
    if (!doc.renameWorkspace(id, label)) {
        return false;
    }
    m_store.setDocument(doc);
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    emit workspacesChanged();
    return true;
}

bool WorkspaceController::deleteWorkspace(const QString& id)
{
    WorkspaceDocument doc = m_store.document();
    const bool wasActive = (doc.activeWorkspace == id);
    if (!doc.removeWorkspace(id)) {
        return false;
    }
    // The store only ever sees the CONSISTENT document — the fallback is
    // already active inside it (review m9: the old sentinel round-trip
    // parked a dangling activeWorkspace in the store, where a crash or a
    // debounce-window write would have persisted it).  The forced switch
    // performs the release/recall against the same target.
    m_store.setDocument(doc);
    if (wasActive && m_enabled) {
        switchWorkspaceInternal(doc.activeWorkspace, /*force=*/true);
    } else {
        if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    }
    emit workspacesChanged();
    return true;
}

bool WorkspaceController::switchWorkspace(const QString& id)
{
    return switchWorkspaceInternal(id, /*force=*/false);
}

bool WorkspaceController::switchWorkspaceInternal(const QString& id, bool force)
{
    WorkspaceDocument doc = m_store.document();
    if (!doc.contains(id)) {
        return false;
    }
    if (!force && doc.activeWorkspace == id) {
        return true;   // the state the caller asked for
    }

    if (!m_enabled) {
        // Mode off: just retarget — the next enable places the new active.
        doc.activeWorkspace = id;
        m_store.setDocument(doc);
        if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
        emit workspacesChanged();
        return true;
    }

    // THE DOCUMENT PIVOTS FIRST (review, K6OZY): releaseAllItems() reaches
    // ContainerManager::saveState() — a real disk flush, deliberately — and
    // between it and the old document write sat the reparent storm.  A kill
    // there left the container layer describing workspace B's closes while
    // the document still named A active, and A came back missing exactly
    // those applets.  activeWorkspace is one atomic setStationValue; with
    // it first, a crash mid-transition boots into B's own recall, which
    // reopens whatever B wants — the layers agree either way.
    doc.activeWorkspace = id;
    m_store.setDocument(doc);
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: switch pivot write failed"
                   << "(read-only session?) — switching in memory only";
    }

    m_applying = true;
    releaseAllItems(/*returnPansToStack=*/false, /*hideBandStack=*/true);

    // FULL RECALL (maintainer ruling): the target workspace decides which
    // applets are open.  Belonging means an item with closed=false; every
    // other OPEN applet closes.  All visibility changes run under
    // m_applying so the visibilityChanged hook does not echo them back
    // into the document.
    QSet<QString> wanted;
    if (const Workspace* ws = doc.workspace(id)) {
        if (const WorkspaceSurface* main = ws->surface(WorkspaceSurface::kMainId)) {
            for (const CanvasItem& it : main->items) {
                if (it.id.startsWith(kAppletItemPrefix) && !it.closed) {
                    wanted.insert(it.id.mid(kAppletItemPrefix.size()));
                }
            }
        }
    }
    // The recall universe is the APPLET CATALOG, never allContainers()
    // (red-team B1): the manager also registers composite children (the
    // Channel Strip's thirteen sub-containers) and the root sidebar, none
    // of which any workspace item can name — iterating them closed every
    // one permanently, with no UI path back.  The catalog is exactly the
    // set the operator can address.  The whole loop runs inside the
    // recall guard so the panel's preference dual-write stays silent
    // (red-team B2) — workspace recall is not the operator editing their
    // Classic preferences.
    // Hardware availability (m_widgetAvailable) is deliberately NOT
    // consulted here: the document is the operator's own recorded intent,
    // and detection can lag connect by seconds — gating recall on it
    // would make a switch racy and recall workspaces incomplete.  Recall
    // outranks availability; only ADDING via the palette is gated
    // (#4968 red-team M2, ruled by the maintainer).
    if (m_panHost.recallGuard) m_panHost.recallGuard(true);
    for (const WidgetCatalogEntry& entry : m_widgetCatalog) {
        ContainerWidget* c = containerForApplet(entry.id);
        if (!c) continue;
        const bool shouldBeOpen = wanted.contains(entry.id);
        if (c->isFloating() && c->isContainerVisible()) {
            continue;   // a pop-out IN USE stays (decision 1)
        }
        // A hidden float participates like anything else (review m8): the
        // operator closed it, and a workspace that wants it open reopens
        // it in the shape it was left — as a float.  Skipping ALL floats
        // let a closed pop-out desert every workspace that listed it.
        if (c->isContainerVisible() != shouldBeOpen) {
            c->setContainerVisible(shouldBeOpen);
        }
    }
    if (m_panHost.recallGuard) m_panHost.recallGuard(false);

    bool docChanged = false;
    placeActiveWorkspaceItems(doc, &docChanged);
    m_applying = false;

    m_undoItemId.clear();   // whole-surface change; a one-rect undo would lie
    m_undoRect = NormRect{};
    m_store.setDocument(doc);
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }

    // Full recall includes the band stack (review M1): a workspace whose
    // document carries the bandstack item gets the panel back at its spot;
    // one without it stays hidden (releaseAllItems hid it above).
    bool wantsBandStack = false;
    if (const Workspace* ws = m_store.document().workspace(id)) {
        if (const WorkspaceSurface* main = ws->surface(WorkspaceSurface::kMainId)) {
            for (const CanvasItem& it : main->items) {
                if (it.id == kBandStackItemId) { wantsBandStack = true; break; }
            }
        }
    }
    if (wantsBandStack) {
        setBandStackVisible(true);
    }

    emit workspacesChanged();
    return true;
}

void WorkspaceController::setWidgetCatalog(const QList<WidgetCatalogEntry>& catalog)
{
    m_widgetCatalog = catalog;
}

void WorkspaceController::setWidgetAvailabilityHook(
    std::function<bool(const QString&)> hook)
{
    m_widgetAvailable = std::move(hook);
}

QList<WorkspaceController::PaletteEntry> WorkspaceController::paletteState() const
{
    QList<PaletteEntry> out;
    out.reserve(m_widgetCatalog.size());
    for (const WidgetCatalogEntry& e : m_widgetCatalog) {
        PaletteEntry p;
        p.id       = e.id;
        p.title    = e.title;
        p.category = e.category;
        ContainerWidget* c = containerForApplet(e.id);
        // Order matters (#4968 red-team M2): an applet ON the canvas is
        // reported as such even while its hardware is undetected —
        // recall outranks availability, so that state is legitimate and
        // the entry must show "placed", not "unavailable".
        if (c && c->isOnCanvas()) {
            p.state = PaletteEntry::State::OnCanvas;
        } else if (!c) {
            p.state = PaletteEntry::State::Absent;
        } else if (m_widgetAvailable && !m_widgetAvailable(e.id)) {
            p.state = PaletteEntry::State::NotDetected;
        } else {
            p.state = PaletteEntry::State::Addable;
        }
        out.append(p);
    }
    return out;
}

bool WorkspaceController::addAppletFromPalette(const QString& appletId,
                                               const QPointF& canvasPos)
{
    if (!m_enabled) {
        return false;   // adding is allowed in BOTH postures (review m3)
    }
    ContainerWidget* c = containerForApplet(appletId);
    if (!c || c->isOnCanvas()) {
        return false;   // absent, or already there — the menu shows why
    }
    if (m_widgetAvailable && !m_widgetAvailable(appletId)) {
        // Hardware not detected — the menu greys these.  Availability
        // gates ADDING only: recall (switchWorkspaceInternal) and
        // placement deliberately ignore it, because the workspace
        // document is the operator's own recorded intent (#4968 M2
        // ruling).
        return false;
    }

    const QString itemId = itemIdFor(c);
    const NormRect rect  = defaultRectFor(c, &canvasPos);

    // Whatever state the applet is in, the palette add ends the same way:
    // an OPEN applet placed at the click point.  The rect is recorded
    // first so every path below places from the document — the same
    // one-placement-path shape import-floats uses.
    writeItemPresence(itemId, QStringLiteral("applet"), rect,
                      /*present=*/true, /*flushNow=*/false);

    if (c->isFloating()) {
        // An explicit palette add outranks decision 1's leave-it-floating:
        // the operator asked for it ON the canvas.
        m_manager->dockContainer(c->id());
    } else if (!c->isContainerVisible()) {
        // Opening fires the visibility hook, which sees the item we just
        // wrote and places at its rect.
        c->setContainerVisible(true);
    } else {
        sendAppletToCanvas(appletId, &rect);
    }
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    return c->isOnCanvas();
}

namespace {
// An import must never bury the whole canvas (review m7): a pop-out
// maximized on a larger monitor maps to w/h ≥ 1.0 after the bounds clamp.
// Cap it below full-surface so the item lands big but grabbable-around.
void capImportRect(NormRect* r)
{
    r->w = qMin(r->w, 0.9);
    r->h = qMin(r->h, 0.9);
}
}  // namespace

int WorkspaceController::importFloatingOntoCanvas()
{
    if (!m_enabled) {
        return 0;
    }
    const QRect canvasGlobal(m_canvas->mapToGlobal(QPoint(0, 0)),
                             m_canvas->size());
    int imported = 0;

    // Floating applet containers: record the mapped rect as the item FIRST,
    // then dock — the existing dockModeChanged hook sees a surviving item
    // and sends the applet to the canvas at exactly that rect.  One
    // placement path, not a special import one.
    for (ContainerWidget* c : m_manager->allContainers()) {
        if (!c || !c->isFloating()) {
            continue;
        }
        QWidget* win = c->window();
        NormRect mapped = normRectFromGlobal(
            win ? win->geometry() : QRect(), canvasGlobal);
        if (!mapped.isValid()) {
            continue;
        }
        capImportRect(&mapped);
        writeItemPresence(itemIdFor(c), QStringLiteral("applet"), mapped,
                          /*present=*/true, /*flushNow=*/false);
        writeItemClosed(itemIdFor(c), false, /*flushNow=*/false);
        m_manager->dockContainer(c->id());
        if (c->isOnCanvas()) {
            ++imported;
        } else {
            // The dock didn't land — do not leave a "present" item for
            // something that is not on the canvas (review m6).
            writeItemPresence(itemIdFor(c), QStringLiteral("applet"),
                              mapped, /*present=*/false, /*flushNow=*/false);
        }
    }

    // Floating pans: same shape through the pan seam — store the mapped
    // rect at the pan's slot, then dock; onPanDocked() places from it.
    if (m_panHost.panIds && m_panHost.isFloating && m_panHost.requestDock) {
        const QStringList pans = m_panHost.panIds();
        for (const QString& panId : pans) {
            if (!m_panHost.isFloating(panId)) {
                continue;
            }
            const QRect floatRect = m_panHost.floatingPanGlobalRect
                                        ? m_panHost.floatingPanGlobalRect(panId)
                                        : QRect();
            NormRect mapped = normRectFromGlobal(floatRect, canvasGlobal);
            if (!mapped.isValid()) {
                continue;   // degenerate window: skipped, like applets (m6)
            }
            capImportRect(&mapped);
            writeItemPresence(panItemIdFor(panId),
                              QStringLiteral("panadapter"), mapped,
                              /*present=*/true, /*flushNow=*/false);
            m_panHost.requestDock(panId);
            // Correct while docking places synchronously (it does — the
            // panDocked emission is direct); a future queued dock would
            // read as zero here and the count must then move to a signal.
            if (m_canvas->contains(panItemIdFor(panId))) {
                ++imported;
            }
        }
    }

    if (imported > 0) {
        if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    }
    return imported;
}

void WorkspaceController::bindProfile(const QString& profileName,
                                      const QString& workspaceId)
{
    if (profileName.trimmed().isEmpty()) {
        return;
    }
    WorkspaceDocument doc = m_store.document();
    if (!doc.contains(workspaceId)) {
        return;
    }
    doc.bindings.insert(profileName, workspaceId);
    m_store.setDocument(doc);
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    emit workspacesChanged();
}

void WorkspaceController::unbindProfile(const QString& profileName)
{
    WorkspaceDocument doc = m_store.document();
    if (doc.bindings.remove(profileName) > 0) {
        m_store.setDocument(doc);
        if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
        emit workspacesChanged();
    }
}

QString WorkspaceController::boundWorkspaceFor(const QString& profileName) const
{
    return m_store.document().boundWorkspace(profileName);
}

void WorkspaceController::onRadioProfileLoaded(const QString& profileType,
                                               const QString& profileName)
{
    // Decisions 6 and 8: a bound GLOBAL profile switches the workspace; an
    // unbound one leaves it alone.  Other profile types (tx/mic) are not
    // operating layouts and never participate.
    if (profileType != QLatin1String("global") || !m_enabled) {
        return;
    }
    const QString target = boundWorkspaceFor(profileName);
    if (target.isEmpty() || target == activeWorkspaceId()) {
        return;
    }
    if (switchWorkspace(target)) {
        const Workspace* ws = m_store.document().workspace(target);
        emit workspaceSwitchedByProfile(
            profileName, ws && !ws->label.isEmpty() ? ws->label : target);
    }
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


}  // namespace AetherSDR
