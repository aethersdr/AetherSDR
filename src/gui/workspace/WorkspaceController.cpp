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
#include <QTimer>
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

// The surface an item BELONGS to per the document; empty when untracked.
QString docSurfaceForItem(const Workspace& ws, const QString& itemId)
{
    for (const WorkspaceSurface& s : ws.surfaces) {
        for (const CanvasItem& it : s.items) {
            if (it.id == itemId) return s.id;
        }
    }
    return QString();
}
}  // namespace

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

    wireCanvas(m_canvas);

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

    // Extra canvas windows open one event-loop turn later: enable() runs
    // inside the shell-swap turn, and creating top-level surfaces there is
    // the wl_subsurface "no parent" hazard the disable path already dodges
    // (the compositor kills the client).  One turn lets Qt commit the
    // reparented tree first; the windows' items place scoped, exactly as
    // a reopened window's do.
    QTimer::singleShot(0, this, [this] {
        if (!m_enabled) return;
        reconcileWindowsWithActiveWorkspace();
        const WorkspaceDocument& d = m_store.document();
        if (const Workspace* w = d.workspace(d.activeWorkspace)) {
            for (const WorkspaceSurface& surf : w->surfaces) {
                if (surf.id != WorkspaceSurface::kMainId && !surf.hidden
                    && canvasForSurface(surf.id)) {
                    placeSurfaceItems(surf.id);
                }
            }
        }
    });

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

    // Per-surface batches (phase 7): each surface restores onto its own
    // canvas.  A hidden surface (window closed, hide-and-keep) or one
    // whose window is not open yet places nothing — its items stay
    // recorded, its pans stay in the (hidden) stack.
    QHash<QString, QList<CanvasItem>> toPlace;
    QHash<QString, QHash<QString, QWidget*>> widgets;
    QStringList crossMovedPans;   // pans placed onto another top level

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

    // ── Normalize: pans BELOW everything else, always — on EVERY surface ─
    //
    // The document's z is otherwise replayed verbatim, and a document that
    // lived through the frontmost-arrival bug (or any future mishap) would
    // resurrect pans over the operator's controls at every boot — the 8600
    // field report's second act.  Pans are the surface the station sits on;
    // enforcing that at replay costs a deliberate pan-over-applet stacking
    // across restarts (nothing supports one today — phase 6's pinning is
    // where that would live) and buys layouts that cannot rot.
    for (WorkspaceSurface& surf : ws->surfaces) {
        QList<CanvasItem> ordered = surf.items;
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
        surf.items = ordered;
        if (zChanged && docChanged) *docChanged = true;
    }

    // ── Pans, from their slot items — routed to their owning surface ─────
    const QStringList livePans =
        m_panHost.panIds ? m_panHost.panIds() : QStringList{};
    for (const QString& panId : livePans) {
        if (m_panHost.isFloating && m_panHost.isFloating(panId)) {
            continue;   // pop-out stays (RFC decision 1)
        }
        const QString itemId = panItemIdFor(panId);
        QString surfId = docSurfaceForItem(*ws, itemId);
        WorkspaceSurface* surf = nullptr;
        for (WorkspaceSurface& candidate : ws->surfaces) {
            if (candidate.id == surfId) { surf = &candidate; break; }
        }
        if (!surf) {
            surf   = main;
            surfId = WorkspaceSurface::kMainId;
        }
        CanvasItem item;
        bool found = false;
        for (const CanvasItem& it : surf->items) {
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
            surf->items.append(item);
            if (docChanged) *docChanged = true;
        }
        // A hidden window's pans stay in the (hidden) stack — the item is
        // kept, nothing is placed (hide-and-keep).
        if (surf->hidden || !canvasForSurface(surfId)) {
            continue;
        }
        item.minimumSize = QSize(320, 180);
        const bool cross = (surfId != WorkspaceSurface::kMainId);
        // A pan headed for another top level takes the floatPanadapter
        // GPU recipe: prepare BEFORE the detach-and-reparent, finish
        // (deferred refresh + show) after restoreItems — the
        // #2495/#4617/#4319 lineage.
        if (cross && m_panHost.prepareTopLevelMove) {
            m_panHost.prepareTopLevelMove(panId);
        }
        QWidget* w = m_panHost.detach ? m_panHost.detach(panId) : nullptr;
        if (!w) continue;
        toPlace[surfId].append(item);
        widgets[surfId].insert(itemId, w);
        if (cross) crossMovedPans.append(panId);
    }

    // ── Applets, per surface ─────────────────────────────────────────────
    for (WorkspaceSurface& surf : ws->surfaces) {
        WorkspaceCanvas* canvas = canvasForSurface(surf.id);
        const bool placeable = !surf.hidden && canvas;
        for (const CanvasItem& item : surf.items) {
            if (!item.id.startsWith(kAppletItemPrefix)) {
                continue;
            }
            ContainerWidget* c =
                containerForApplet(item.id.mid(kAppletItemPrefix.size()));
            if (!placeable) {
                // Hide-and-keep: an applet recorded on a closed window is
                // transiently shut so it cannot sit open-but-invisible
                // behind the hidden panel.  No document write — the item
                // and its closed flag are exactly as recorded.
                if (c && !item.closed && c->isContainerVisible()
                    && !c->isFloating()) {
                    if (m_panHost.recallGuard) m_panHost.recallGuard(true);
                    c->setContainerVisible(false);
                    if (m_panHost.recallGuard) m_panHost.recallGuard(false);
                }
                continue;
            }
            // A closed-flagged item belongs to the workspace but its applet
            // is shut (phase 6 full recall) — placement skips it.  UNLESS
            // the applet is currently OPEN: it was reopened while the mode
            // was off (the visibility hook early-returns there), and the
            // operator's click outranks the stale flag (review, K6OZY) —
            // clear and place.
            if (item.closed) {
                if (!c || !c->isContainerVisible() || c->isFloating()) {
                    continue;
                }
                for (CanvasItem& live : surf.items) {
                    if (live.id == item.id) {
                        live.closed = false;
                        if (docChanged) *docChanged = true;
                        break;
                    }
                }
            }
            // Closed applets keep their home but are not placed; floating
            // ones stay out (pop-out stays, RFC decision 1) until they dock.
            if (!c || !c->isContainerVisible() || c->isFloating()) {
                continue;
            }
            if (m_manager->detachForCanvas(c->id()) != c) {
                continue;
            }
            toPlace[surf.id].append(item);
            widgets[surf.id].insert(item.id, c);
        }
    }

    for (auto it = toPlace.constBegin(); it != toPlace.constEnd(); ++it) {
        if (WorkspaceCanvas* canvas = canvasForSurface(it.key())) {
            canvas->restoreItems(it.value(), widgets.value(it.key()));
        }
    }
    for (const QString& panId : crossMovedPans) {
        if (m_panHost.finishTopLevelMove) m_panHost.finishTopLevelMove(panId);
    }
}

void WorkspaceController::disable()
{
    if (!m_enabled) {
        return;
    }

    m_applying = true;
    releaseAllItems(/*returnPansToStack=*/true, /*hideBandStack=*/false);
    m_applying = false;

    // Reopen what hide-and-keep transiently closed (red-team #4971 M2):
    // an applet recorded open on a HIDDEN window is on no canvas, so the
    // release sweep above never touched it — and Classic came up with an
    // applet missing that Applet_<ID> says is open.  Disabling is not a
    // statement about any applet; give them back to the panel, silently.
    {
        const WorkspaceDocument& d = m_store.document();
        if (const Workspace* ws = d.workspace(d.activeWorkspace)) {
            if (m_panHost.recallGuard) m_panHost.recallGuard(true);
            m_applying = true;
            for (const WorkspaceSurface& surf : ws->surfaces) {
                if (!surf.hidden) continue;
                for (const CanvasItem& it : surf.items) {
                    if (!it.id.startsWith(kAppletItemPrefix) || it.closed) {
                        continue;
                    }
                    ContainerWidget* c = containerForApplet(
                        it.id.mid(kAppletItemPrefix.size()));
                    if (c && !c->isContainerVisible() && !c->isFloating()) {
                        c->setContainerVisible(true);
                    }
                }
            }
            m_applying = false;
            if (m_panHost.recallGuard) m_panHost.recallGuard(false);
        }
    }

    // Extra canvas windows close with the mode (their hidden flags are
    // untouched — disabling is not a statement about any window); their
    // geometry hints are captured first, since hiding is how they would
    // otherwise be lost.
    const QStringList openWindows = m_extraCanvases.keys();
    for (const QString& sid : openWindows) {
        if (m_windowHost.geometryHint) {
            noteWindowGeometryHint(sid, m_windowHost.geometryHint(sid));
        }
        m_extraCanvases.remove(sid);
        if (m_windowHost.closeWindow) m_windowHost.closeWindow(sid);
    }

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

void WorkspaceController::setWindowHost(const WindowHostHooks& hooks)
{
    m_windowHost = hooks;
}

// ── Multi-surface plumbing (phase 7) ─────────────────────────────────────

void WorkspaceController::wireCanvas(WorkspaceCanvas* canvas)
{
    if (m_wiredCanvases.contains(canvas)) {
        return;   // a reopened window hands back the same canvas object
    }
    m_wiredCanvases.insert(canvas);
    // Forget the pointer the moment the canvas dies — whatever deletes it
    // (removeCanvasWindow, resetToClassic, shutdown, or the host's own
    // teardown).  A destroyed-watch beats manual removal at every destroy
    // site: red-team #4971 M1 found hidden windows deleted through paths
    // no manual remove covered, and a recycled allocation landing on a
    // stale set entry comes up dead (e85f9c81).
    connect(canvas, &QObject::destroyed, this,
            [this, canvas] { m_wiredCanvases.remove(canvas); });
    canvas->setDropMimeType(kAppletMime);

    connect(canvas, &WorkspaceCanvas::dropReceived, this,
            [this, canvas](const QString& payload, const QPointF& pos) {
                onDropReceived(canvas, payload, pos);
            });
    connect(canvas, &WorkspaceCanvas::itemRectChanged,
            this, &WorkspaceController::onItemRectChanged);
    connect(canvas, &WorkspaceCanvas::itemStackingChanged,
            this, [this](const QString&) {
                if (m_applying || !m_enabled) return;
                writeStackingFromCanvas();
            });

    // Gestures (phase 5): snapshot for undo at the start, flush the debounced
    // rect stream at the end — the auto-commit gesture boundary.
    connect(canvas, &WorkspaceCanvas::gestureStarted,
            this, [this](const QString& itemId, const NormRect& startRect) {
                if (m_applying || !m_enabled) return;
                m_undoItemId = itemId;
                m_undoRect   = startRect;
            });
    connect(canvas, &WorkspaceCanvas::gestureFinished,
            this, [this](const QString&) {
                if (m_applying || !m_enabled) return;
                m_store.flush();
            });
    connect(canvas, &WorkspaceCanvas::itemDraggedOut,
            this, &WorkspaceController::onItemDraggedOut);
    connect(canvas, &WorkspaceCanvas::contextMenuRequested, this,
            [this, canvas](const QString& itemId, const QPoint& globalPos) {
                onContextMenuRequested(canvas, itemId, globalPos);
            });

    // Edit posture is CONTROLLER-wide (one Edit Layout toggles every
    // surface): mirror any canvas's flip onto all the others, guarded
    // against the echo.
    connect(canvas, &WorkspaceCanvas::editModeChanged, this,
            [this](bool on) {
                if (m_syncingEditMode) return;
                m_syncingEditMode = true;
                for (WorkspaceCanvas* c : attachedCanvases()) {
                    if (c->isEditMode() != on) c->setEditMode(on);
                }
                m_syncingEditMode = false;
            });
}

QList<WorkspaceCanvas*> WorkspaceController::attachedCanvases() const
{
    QList<WorkspaceCanvas*> out;
    out.append(m_canvas);
    for (auto it = m_extraCanvases.constBegin();
         it != m_extraCanvases.constEnd(); ++it) {
        if (it.value()) out.append(it.value().data());
    }
    return out;
}

WorkspaceCanvas* WorkspaceController::canvasForSurface(const QString& surfaceId) const
{
    if (surfaceId.isEmpty() || surfaceId == WorkspaceSurface::kMainId) {
        return m_canvas;
    }
    return m_extraCanvases.value(surfaceId).data();
}

WorkspaceCanvas* WorkspaceController::canvasHolding(const QString& itemId) const
{
    for (WorkspaceCanvas* c : attachedCanvases()) {
        if (c->contains(itemId)) return c;
    }
    return nullptr;
}

QString WorkspaceController::surfaceOf(const WorkspaceCanvas* canvas) const
{
    if (canvas == m_canvas) {
        return WorkspaceSurface::kMainId;
    }
    for (auto it = m_extraCanvases.constBegin();
         it != m_extraCanvases.constEnd(); ++it) {
        if (it.value().data() == canvas) return it.key();
    }
    return WorkspaceSurface::kMainId;
}

QString WorkspaceController::surfaceHosting(const QString& itemId) const
{
    for (WorkspaceCanvas* c : attachedCanvases()) {
        if (c->contains(itemId)) return surfaceOf(c);
    }
    return QString();
}

void WorkspaceController::reconcileWindowsWithActiveWorkspace(bool reapplyHints)
{
    const WorkspaceDocument& doc = m_store.document();
    const Workspace* ws = doc.workspace(doc.activeWorkspace);

    // Close (hide) every open window the target does not want open.  The
    // widgets were already released by the caller.
    const QStringList openIds = m_extraCanvases.keys();
    for (const QString& sid : openIds) {
        bool wanted = false;
        if (ws) {
            for (const WorkspaceSurface& s : ws->surfaces) {
                if (s.id == sid && !s.hidden) { wanted = true; break; }
            }
        }
        if (!wanted) {
            m_extraCanvases.remove(sid);
            if (m_windowHost.closeWindow) m_windowHost.closeWindow(sid);
        }
    }
    if (!ws) {
        return;
    }
    // Open the target's visible extra surfaces and attach their canvases.
    for (const WorkspaceSurface& s : ws->surfaces) {
        if (s.id == WorkspaceSurface::kMainId || s.hidden) {
            continue;
        }
        if (m_extraCanvases.value(s.id)) {
            if (m_windowHost.setWindowLabel) {
                m_windowHost.setWindowLabel(s.id, s.label);
            }
            // On a workspace SWITCH the same surface id reuses the open
            // window, but each workspace records its own geometry — apply
            // the target's hint (red-team #4971 L2).  Not on every
            // reconcile: re-applying a possibly-stale stored hint outside
            // a switch would snap back a window the operator just dragged
            // (the debounce is 400 ms wide).
            if (reapplyHints && m_windowHost.applyGeometryHint) {
                m_windowHost.applyGeometryHint(s.id, s.windowGeometry);
            }
            continue;
        }
        if (!m_windowHost.openWindow) {
            continue;
        }
        WorkspaceCanvas* canvas =
            m_windowHost.openWindow(s.id, s.label, s.windowGeometry);
        if (!canvas) {
            continue;
        }
        wireCanvas(canvas);   // idempotent per canvas object
        m_extraCanvases.insert(s.id, canvas);
        // Posture follows the controller, and locked is the operating
        // default — same rule as enable().
        if (canvas->isEditMode() != m_canvas->isEditMode()) {
            m_syncingEditMode = true;
            canvas->setEditMode(m_canvas->isEditMode());
            m_syncingEditMode = false;
        }
    }
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

    // The pan belongs to whichever surface its item is recorded on
    // (phase 7); untracked pans land on main.  A hidden surface keeps the
    // pan in the stack — hide-and-keep.
    QString surfId = WorkspaceSurface::kMainId;
    bool haveStored = false;
    NormRect rect{0.2, 0.2, 0.6, 0.6};
    const WorkspaceDocument& doc = m_store.document();
    if (const Workspace* ws = doc.workspace(doc.activeWorkspace)) {
        const QString owner = docSurfaceForItem(*ws, itemId);
        if (!owner.isEmpty()) {
            surfId = owner;
            if (const WorkspaceSurface* surf = ws->surface(owner)) {
                if (surf->hidden) {
                    return false;   // window closed; the item stands
                }
                for (const CanvasItem& it : surf->items) {
                    if (it.id == itemId) {
                        rect       = it.rect;
                        haveStored = true;
                        break;
                    }
                }
            }
        }
    }
    WorkspaceCanvas* canvas = canvasForSurface(surfId);
    if (!canvas) {
        return false;   // window not open yet; placement follows it
    }
    if (canvas->contains(itemId)) {
        QWidget* placed = canvas->itemWidget(itemId);
        if (placed && placed->parentWidget() == canvas) {
            return true;   // genuinely placed — the state the caller asked for
        }
        // The entry is a lie: the widget was reclaimed behind the canvas's
        // back (a stack rebuild that predates the loan set, or any future
        // path that forgets it).  Heal instead of trusting: drop the stale
        // entry and fall through to a fresh detach-and-place.
        canvas->releaseItem(itemId);
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

    const bool cross = (surfId != WorkspaceSurface::kMainId);
    if (cross && m_panHost.prepareTopLevelMove) {
        m_panHost.prepareTopLevelMove(panId);   // the GPU recipe (#4617)
    }
    QWidget* w = m_panHost.detach ? m_panHost.detach(panId) : nullptr;
    if (!w) {
        return false;
    }
    if (!canvas->addItem(itemId, w, rect, QStringLiteral("panadapter"),
                         QSize(320, 180))) {
        if (m_panHost.restore) {
            m_panHost.restore(panId, w);   // never strand a detached applet
        }
        if (cross && m_panHost.finishTopLevelMove) {
            m_panHost.finishTopLevelMove(panId);
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
    canvas->sendItemToBack(itemId);
    if (!haveStored) {
        int otherPans = 0;
        for (const CanvasItem& it : canvas->layout().itemsByZ()) {
            if (it.id != itemId && it.id.startsWith(kPanItemPrefix)) {
                ++otherPans;
            }
        }
        for (int i = 0; i < otherPans; ++i) {
            canvas->raiseItem(itemId);
        }
    }
    if (cross && m_panHost.finishTopLevelMove) {
        m_panHost.finishTopLevelMove(panId);
    }
    writeItemPresence(itemId, QStringLiteral("panadapter"),
                      canvas->itemRect(itemId), /*present=*/true,
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
        const QString itemId = kPanItemPrefix + QString::number(it.value());
        if (WorkspaceCanvas* canvas = canvasHolding(itemId)) {
            canvas->releaseItem(itemId);
        }
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
        const QString itemId = kPanItemPrefix + QString::number(it.value());
        if (WorkspaceCanvas* canvas = canvasHolding(itemId)) {
            canvas->releaseItem(itemId);
        }
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
    if (WorkspaceCanvas* canvas = canvasHolding(itemId)) {
        canvas->beginMoveGesture(itemId, globalPos);
        m_gestureCanvas = canvas;
    }
}

void WorkspaceController::movePanItem(const QPoint& globalPos)
{
    if (m_enabled && m_gestureCanvas) {
        m_gestureCanvas->moveGesture(globalPos);
    }
}

void WorkspaceController::endPanItemMove(const QPoint& globalPos)
{
    if (m_enabled && m_gestureCanvas) {
        m_gestureCanvas->endGesture(globalPos);
        m_gestureCanvas = nullptr;
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

    // The applet belongs to whichever surface its item is recorded on
    // (phase 7); untracked ones land on main.  A hidden surface keeps the
    // applet off-canvas — hide-and-keep.
    QString surfId = WorkspaceSurface::kMainId;
    // Placement priority: the caller's rect (a drop point), else the
    // document's remembered home, else a default sized from the widget.
    NormRect rect;
    bool haveStored = false;
    {
        const WorkspaceDocument& doc = m_store.document();
        if (const Workspace* ws = doc.workspace(doc.activeWorkspace)) {
            const QString owner = docSurfaceForItem(*ws, itemId);
            if (!owner.isEmpty()) {
                surfId = owner;
                if (const WorkspaceSurface* surf = ws->surface(owner)) {
                    if (surf->hidden) {
                        return false;   // window closed; the item stands
                    }
                    for (const CanvasItem& it : surf->items) {
                        if (it.id == itemId) {
                            rect       = it.rect;
                            haveStored = true;
                            break;
                        }
                    }
                }
            }
        }
    }
    WorkspaceCanvas* canvas = canvasForSurface(surfId);
    if (!canvas) {
        return false;   // window not open yet; placement follows it
    }
    if (where) {
        rect = *where;
    } else if (!haveStored) {
        rect = defaultRectFor(c, nullptr);
    }

    if (m_manager->detachForCanvas(c->id()) != c) {
        return false;
    }
    if (!canvas->addItem(itemId, c, rect, QStringLiteral("applet"))) {
        // Should not happen (id collisions are checked above), but never
        // strand a detached widget: put it straight back.
        m_manager->returnFromCanvas(c->id(), c);
        return false;
    }

    // The canvas clamped the rect; persist what is actually on screen.
    writeItemPresence(itemId, QStringLiteral("applet"),
                      canvas->itemRect(itemId), /*present=*/true,
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

    if (WorkspaceCanvas* canvas = canvasHolding(itemId)) {
        canvas->takeItem(itemId);
    }
    m_manager->returnFromCanvas(c->id(), c);
}

// ── Canvas events ────────────────────────────────────────────────────────

void WorkspaceController::onDropReceived(WorkspaceCanvas* canvas,
                                         const QString& payload,
                                         const QPointF& pos)
{
    if (!m_enabled) {
        return;
    }
    ContainerWidget* c = containerForApplet(payload);
    if (!c) {
        return;
    }
    const QString itemId = itemIdFor(c);

    if (c->isOnCanvas()) {
        WorkspaceCanvas* holder = canvasHolding(itemId);
        if (holder && holder != canvas) {
            // Dragged from one window's panel strip onto ANOTHER canvas:
            // that is a move between surfaces (phase 7), through the one
            // path that owns cross-top-level reparents.
            moveItemToSurface(itemId, surfaceOf(canvas));
            holder = canvasHolding(itemId);
        }
        if (!holder) {
            return;
        }
        // Move: keep the size, centre the item on the drop point, and let
        // the canvas clamp.  itemRectChanged writes the document; this is a
        // discrete gesture, so flush behind it.
        const NormRect cur = holder->itemRect(itemId);
        NormRect moved     = cur;
        moved.x            = pos.x() - cur.w / 2.0;
        moved.y            = pos.y() - cur.h / 2.0;
        holder->setItemRect(itemId, moved);
        m_store.flush();
        return;
    }

    // A drop lands on the canvas it was dropped on: record the home there
    // first, then send — sendAppletToCanvas routes by the document.
    NormRect rect = defaultRectFor(c, &pos, canvas);
    writeItemPresence(itemId, QStringLiteral("applet"), rect,
                      /*present=*/true, /*flushNow=*/false,
                      surfaceOf(canvas));
    sendAppletToCanvas(appletIdFor(c), &rect);
    m_store.flush();
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
                if (!m_enabled || !c->isOnCanvas()) return;
                if (WorkspaceCanvas* canvas = canvasHolding(itemIdFor(c))) {
                    canvas->beginMoveGesture(itemIdFor(c), g);
                    m_gestureCanvas = canvas;
                }
            });
    connect(c, &ContainerWidget::canvasDragMoved, this,
            [this, c](const QPoint& g) {
                if (m_enabled && c->isOnCanvas() && m_gestureCanvas) {
                    m_gestureCanvas->moveGesture(g);
                }
            });
    connect(c, &ContainerWidget::canvasDragEnded, this,
            [this, c](const QPoint& g) {
                if (m_enabled && c->isOnCanvas() && m_gestureCanvas) {
                    m_gestureCanvas->endGesture(g);
                    m_gestureCanvas = nullptr;
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
                if (!docSurfaceForItem(*ws, itemIdFor(c)).isEmpty()) {
                    // sendAppletToCanvas's writeItemPresence clears the
                    // closed flag — the invariant lives there — and the
                    // send routes to the item's own surface (phase 7).
                    sendAppletToCanvas(appletIdFor(c));
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
                    if (!docSurfaceForItem(*ws, itemIdFor(c)).isEmpty()) {
                        sendAppletToCanvas(appletIdFor(c));
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
        // Item ids are workspace-wide identity: whichever surface holds
        // the item is where the rect lands (phase 7).
        for (WorkspaceSurface& s : ws.surfaces) {
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
                                            bool flushNow,
                                            const QString& surfaceId)
{
    WorkspaceDocument doc = m_store.document();
    for (Workspace& ws : doc.workspaces) {
        if (ws.id != doc.activeWorkspace) continue;

        // Find the surface that currently holds the item, if any.
        int surfIdx = -1, itemIdx = -1;
        for (int si = 0; si < ws.surfaces.size(); ++si) {
            for (int ii = 0; ii < ws.surfaces.at(si).items.size(); ++ii) {
                if (ws.surfaces.at(si).items.at(ii).id == itemId) {
                    surfIdx = si;
                    itemIdx = ii;
                    break;
                }
            }
            if (surfIdx >= 0) break;
        }

        if (!present) {
            if (surfIdx >= 0) {
                ws.surfaces[surfIdx].items.removeAt(itemIdx);
                m_store.setDocument(doc);
                if (flushNow) m_store.flush();
            }
            return;   // removing something untracked is a no-op
        }

        // An EXPLICIT surfaceId is the caller stating intent — it
        // RELOCATES an existing entry (red-team #4971 B1: the palette's
        // target surface was silently discarded whenever the applet
        // already had a home, so 'Add widget' on a canvas window placed
        // on the wrong surface at the wrong size).  An IMPLICIT write
        // (empty surfaceId) updates in place: presence is not a move.
        QString targetSurface = surfaceId;
        if (targetSurface.isEmpty()) {
            targetSurface = (surfIdx >= 0) ? ws.surfaces.at(surfIdx).id
                                           : surfaceHosting(itemId);
        }
        if (targetSurface.isEmpty() || !ws.surface(targetSurface)) {
            targetSurface = WorkspaceSurface::kMainId;
        }

        CanvasItem item;
        if (surfIdx >= 0) {
            item = ws.surfaces.at(surfIdx).items.at(itemIdx);
            if (ws.surfaces.at(surfIdx).id == targetSurface) {
                // Making an item PRESENT is the one statement that its
                // applet is wanted on the surface — the closed flag
                // clears here, in one place, instead of at whichever
                // call sites remembered to (review, K6OZY: two of four
                // compensated by hand and the reopen-while-disabled
                // path cleared nothing).
                ws.surfaces[surfIdx].items[itemIdx].rect   = rect;
                ws.surfaces[surfIdx].items[itemIdx].closed = false;
                m_store.setDocument(doc);
                if (flushNow) m_store.flush();
                return;
            }
            ws.surfaces[surfIdx].items.removeAt(itemIdx);
        } else {
            item.id          = itemId;
            item.contentType = contentType;
        }
        item.rect   = rect;
        item.closed = false;
        {
            WorkspaceCanvas* canvas = canvasForSurface(targetSurface);
            // Never below zero (red-team #4971 L4): the widget lands
            // after this write, so zOf() answers -1 and a fresh applet
            // sorted below the pans at replay.
            item.z = canvas ? qMax(0, canvas->layout().zOf(itemId)) : 0;
        }
        for (WorkspaceSurface& s : ws.surfaces) {
            if (s.id != targetSurface) continue;
            s.items.append(item);
            break;
        }
        m_store.setDocument(doc);
        if (flushNow) m_store.flush();
        return;
    }
}

void WorkspaceController::writeStackingFromCanvas()
{
    WorkspaceDocument doc = m_store.document();
    for (Workspace& ws : doc.workspaces) {
        if (ws.id != doc.activeWorkspace) continue;
        for (WorkspaceSurface& s : ws.surfaces) {
            WorkspaceCanvas* canvas = canvasForSurface(s.id);
            if (!canvas) continue;   // hidden window: stored z stands
            for (CanvasItem& it : s.items) {
                const int z = canvas->layout().zOf(it.id);
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
    WorkspaceCanvas* canvas = canvasHolding(m_undoItemId);
    if (!canvas) {
        m_undoItemId.clear();
        return false;
    }
    // Swap current and remembered: undoing twice toggles, which is the
    // single-slot version of redo.
    const NormRect current = canvas->itemRect(m_undoItemId);
    canvas->setItemRect(m_undoItemId, m_undoRect);
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

    // Everything off EVERY surface: applets back to their panel slots
    // (open), main-canvas pans merely released (they stay parented; the
    // re-place below re-adds them — no nullptr detour, #1344), extra-
    // window pans back to the stack through the GPU recipe, the band
    // stack re-homed and hidden (Classic is the stock shell, and the
    // stock shell shows none).
    releaseAllItems(/*returnPansToStack=*/false, /*hideBandStack=*/true);

    // Classic is re-derived from the same legacy keys the first enable
    // migrated from — the panel still dual-writes them, so this reflects
    // the operator's CURRENT open applets and order, not a stale snapshot.
    // One definition of Classic, used everywhere (WorkspaceMigration).
    WorkspaceDocument doc = m_store.document();

    // Classic is the STOCK shell, and the stock shell is one window: the
    // active workspace's extra surfaces collapse (phase 7).  removeSurface
    // parks their items on main, where the fresh Classic composition below
    // overwrites them — reset is authoritative, that is its whole promise
    // ("back to a sane shell in one action").
    {
        QStringList extras;
        if (const Workspace* ws = doc.workspace(doc.activeWorkspace)) {
            for (const WorkspaceSurface& surf : ws->surfaces) {
                if (surf.id != WorkspaceSurface::kMainId) {
                    extras.append(surf.id);
                }
            }
        }
        for (const QString& sid : extras) {
            m_extraCanvases.remove(sid);
            // Unconditional — hidden windows too (red-team #4971 M1).
            if (m_windowHost.destroyWindow) m_windowHost.destroyWindow(sid);
            doc.removeSurface(doc.activeWorkspace, sid);
        }
    }
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
    emit workspacesChanged();   // the window list may have collapsed
}

void WorkspaceController::tidyLayout(WorkspaceCanvas* canvas)
{
    if (!m_enabled) {
        return;
    }
    if (!canvas) {
        canvas = m_canvas;
    }

    QList<CanvasItem> items;
    QStringList fixedIds{kPanStackItemId};
    for (const CanvasItem& it : canvas->layout().itemsByZ()) {
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
        canvas->setItemRect(mv.id, mv.rect);   // rect stream → touch
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

void WorkspaceController::onContextMenuRequested(WorkspaceCanvas* canvas,
                                                 const QString& itemId,
                                                 const QPoint& globalPos)
{
    if (!m_enabled || !canvas) {
        return;
    }

    QMenu menu;

    // Edit Layout leads in BOTH postures — it is the door between them —
    // and everything below it is placement, so a locked canvas shows only
    // the door.  Posture is controller-wide; any canvas answers for all.
    QAction* editToggle = menu.addAction(QStringLiteral("Edit layout"));
    editToggle->setCheckable(true);
    editToggle->setChecked(canvas->isEditMode());
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
            canvas->rect().isEmpty()
                ? QPointF(0.5, 0.5)
                : QPointF(canvas->mapFromGlobal(globalPos).x()
                              / double(canvas->width()),
                          canvas->mapFromGlobal(globalPos).y()
                              / double(canvas->height()));
        const QString menuSurface = surfaceOf(canvas);

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
                            [this, appletId, title, canvasPos, globalPos,
                             menuSurface] {
                                if (!addAppletFromPalette(appletId,
                                                          canvasPos,
                                                          menuSurface)) {
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


    if (!canvas->isEditMode()) {
        menu.exec(globalPos);
        return;
    }
    menu.addSeparator();

    const bool onApplet =
        !itemId.isEmpty() && itemId.startsWith(kAppletItemPrefix);
    const bool onPan = !itemId.isEmpty() && itemId.startsWith(kPanItemPrefix);

    // Move to ▸ (phase 7, maintainer ruling: the menu IS the move
    // mechanism this phase) — one deliberate action through the one path
    // that owns cross-top-level reparents.  Applets and pans only; the
    // band stack and the phase-3 panstack are shell furniture.
    if (onApplet || onPan) {
        QMenu* moveMenu = menu.addMenu(QStringLiteral("Move to"));
        const QString here = surfaceOf(canvas);
        if (here != WorkspaceSurface::kMainId) {
            moveMenu->addAction(QStringLiteral("Main window"), this,
                                [this, itemId] {
                                    moveItemToSurface(
                                        itemId, WorkspaceSurface::kMainId);
                                });
        }
        for (const CanvasWindowInfo& info : canvasWindowList()) {
            if (info.id == here) continue;
            const QString sid = info.id;
            // A hidden window is a legal target: moving something to it
            // reopens it (moveItemToSurface clears the flag).
            moveMenu->addAction(
                menuText(info.label)
                    + (info.open ? QString() : tr(" (closed)")),
                this, [this, itemId, sid] {
                    moveItemToSurface(itemId, sid);
                });
        }
        moveMenu->addSeparator();
        moveMenu->addAction(QStringLiteral("New canvas window"), this,
                            [this, itemId] {
                                const QString sid =
                                    addCanvasWindow(QString());
                                if (!sid.isEmpty()) {
                                    moveItemToSurface(itemId, sid);
                                }
                            });
        menu.addSeparator();
    }

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
                       [canvas, itemId] { canvas->sendItemToBack(itemId); });
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
                       [canvas, itemId] { canvas->bringItemToFront(itemId); });
        menu.addAction(QStringLiteral("Send to back"), this,
                       [canvas, itemId] { canvas->sendItemToBack(itemId); });
        menu.addSeparator();
    }

    QAction* gridSnap = menu.addAction(QStringLiteral("Snap to grid"));
    gridSnap->setCheckable(true);
    gridSnap->setChecked(canvas->isGridSnapEnabled());
    connect(gridSnap, &QAction::toggled, this,
            [canvas](bool on) { canvas->setGridSnapEnabled(on); });

    QAction* undo = menu.addAction(QStringLiteral("Undo last placement"), this,
                                   [this] { undoLastPlacement(); });
    undo->setEnabled(canUndo());
    menu.addAction(QStringLiteral("Tidy layout"), this,
                   [this, canvas] { tidyLayout(canvas); });
    menu.addAction(QStringLiteral("Reset layout to Classic"), this,
                   [this] { resetToClassic(); });

    menu.exec(globalPos);
}

void WorkspaceController::releaseAllItems(bool returnPansToStack,
                                          bool hideBandStack)
{
    for (WorkspaceCanvas* canvas : attachedCanvases()) {
        // A pan leaving an EXTRA canvas crosses top levels either way —
        // back to the stack now, or detached again by the placement that
        // follows — so it always returns to the stack here, wrapped in
        // the GPU recipe.  Main-canvas pans keep the fast path: on a
        // switch they stay parented and the re-place is same-top-level.
        const bool cross = (canvas != m_canvas);
        const QStringList ids = canvas->layout().ids();
        for (const QString& itemId : ids) {
            if (itemId.startsWith(kPanItemPrefix)) {
                // Release, never take: the restore hook reparents in ONE
                // step (addWidget), and the nullptr detour is forbidden
                // for QRhi children (#1344).
                const QString panId = panIdForItem(itemId);
                const bool toStack = returnPansToStack || cross;
                if (cross && !panId.isEmpty()
                    && m_panHost.prepareTopLevelMove) {
                    m_panHost.prepareTopLevelMove(panId);
                }
                QWidget* w = canvas->releaseItem(itemId);
                if (toStack && w && !panId.isEmpty() && m_panHost.restore) {
                    m_panHost.restore(panId, w);
                }
                if (cross && !panId.isEmpty()
                    && m_panHost.finishTopLevelMove) {
                    m_panHost.finishTopLevelMove(panId);
                }
                continue;
            }
            if (itemId == kBandStackItemId) {
                QWidget* w = canvas->releaseItem(itemId);
                if (w && m_panHost.reclaimBandStack) {
                    m_panHost.reclaimBandStack(w);
                }
                if (w && hideBandStack) {
                    w->hide();   // session-transient; a switch starts it hidden
                }
                continue;
            }
            if (itemId == kPanStackItemId) {
                // Phase-3 document robustness: released parentless;
                // MainWindow puts it back in the splitter.
                canvas->takeItem(itemId);
                continue;
            }
            QWidget* w = canvas->takeItem(itemId);
            if (auto* c = qobject_cast<ContainerWidget*>(w)) {
                m_manager->returnFromCanvas(c->id(), c);
            }
        }
    }
}

// ── Additional canvas windows (RFC #4887 phase 7) ────────────────────────

void WorkspaceController::evictSurfaceTransiently(const QString& surfaceId)
{
    WorkspaceCanvas* canvas = canvasForSurface(surfaceId);
    if (!canvas || canvas == m_canvas) {
        return;   // the main surface never evicts wholesale
    }
    const bool wasApplying = m_applying;   // nest-safe (review L6)
    m_applying = true;
    if (m_panHost.recallGuard) m_panHost.recallGuard(true);
    const QStringList ids = canvas->layout().ids();
    for (const QString& itemId : ids) {
        if (itemId.startsWith(kPanItemPrefix)) {
            const QString panId = panIdForItem(itemId);
            if (!panId.isEmpty() && m_panHost.prepareTopLevelMove) {
                m_panHost.prepareTopLevelMove(panId);
            }
            QWidget* w = canvas->releaseItem(itemId);
            if (w && !panId.isEmpty() && m_panHost.restore) {
                m_panHost.restore(panId, w);   // back to the hidden stack
            }
            if (!panId.isEmpty() && m_panHost.finishTopLevelMove) {
                m_panHost.finishTopLevelMove(panId);
            }
            continue;
        }
        QWidget* w = canvas->takeItem(itemId);
        if (auto* c = qobject_cast<ContainerWidget*>(w)) {
            m_manager->returnFromCanvas(c->id(), c);
            // Transiently closed (hide-and-keep): no document write, so
            // the item and its closed flag stay exactly as recorded and
            // reopening the window restores this applet open.  The guard
            // keeps the panel's Applet_<ID> dual-write silent too.
            c->setContainerVisible(false);
        }
    }
    if (m_panHost.recallGuard) m_panHost.recallGuard(false);
    m_applying = wasApplying;
}

void WorkspaceController::placeSurfaceItems(const QString& surfaceId)
{
    WorkspaceCanvas* canvas = canvasForSurface(surfaceId);
    if (!canvas) {
        return;
    }
    WorkspaceDocument doc = m_store.document();
    const Workspace* ws = doc.workspace(doc.activeWorkspace);
    const WorkspaceSurface* surf = ws ? ws->surface(surfaceId) : nullptr;
    if (!surf || surf->hidden) {
        return;
    }

    const bool wasApplying = m_applying;   // nest-safe (review L6)
    m_applying = true;
    if (m_panHost.recallGuard) m_panHost.recallGuard(true);

    QList<CanvasItem> toPlace;
    QHash<QString, QWidget*> widgets;
    QStringList crossPans;
    const bool cross = (surfaceId != WorkspaceSurface::kMainId);

    for (const CanvasItem& item : surf->items) {
        if (item.id.startsWith(kPanItemPrefix)) {
            const QString panId = panIdForItem(item.id);
            if (panId.isEmpty()) continue;   // no live pan in this slot
            if (m_panHost.isFloating && m_panHost.isFloating(panId)) {
                continue;   // pop-out stays (RFC decision 1)
            }
            if (canvas->contains(item.id)) continue;
            if (cross && m_panHost.prepareTopLevelMove) {
                m_panHost.prepareTopLevelMove(panId);
            }
            QWidget* w = m_panHost.detach ? m_panHost.detach(panId) : nullptr;
            if (!w) continue;
            CanvasItem placed = item;
            placed.minimumSize = QSize(320, 180);
            toPlace.append(placed);
            widgets.insert(item.id, w);
            if (cross) crossPans.append(panId);
            continue;
        }
        if (!item.id.startsWith(kAppletItemPrefix) || item.closed) {
            continue;
        }
        ContainerWidget* c =
            containerForApplet(item.id.mid(kAppletItemPrefix.size()));
        if (!c || c->isOnCanvas() || c->isFloating()) {
            continue;
        }
        // Reopen the transient close the hide performed — under the guard,
        // so neither the document nor the panel's preferences hear it.
        if (!c->isContainerVisible()) {
            c->setContainerVisible(true);
        }
        if (m_manager->detachForCanvas(c->id()) != c) {
            continue;
        }
        toPlace.append(item);
        widgets.insert(item.id, c);
    }

    canvas->restoreItems(toPlace, widgets);
    for (const QString& panId : crossPans) {
        if (m_panHost.finishTopLevelMove) m_panHost.finishTopLevelMove(panId);
    }

    if (m_panHost.recallGuard) m_panHost.recallGuard(false);
    m_applying = wasApplying;
}

QString WorkspaceController::addCanvasWindow(const QString& label)
{
    if (!m_enabled || !m_store.isLoaded()) {
        return QString();
    }
    WorkspaceDocument doc = m_store.document();
    const QString sid = doc.addSurface(doc.activeWorkspace, label);
    if (sid.isEmpty()) {
        return QString();
    }
    m_store.setDocument(doc);
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    reconcileWindowsWithActiveWorkspace();
    // Creating is arranging — the same reasoning as createWorkspace(): a
    // fresh, empty window greets the operator ready to receive widgets.
    if (canvasForSurface(sid)) {
        m_canvas->setEditMode(true);   // mirrored to every surface
    }
    emit workspacesChanged();
    return sid;
}

bool WorkspaceController::removeCanvasWindow(const QString& surfaceId)
{
    if (!m_enabled || surfaceId == WorkspaceSurface::kMainId) {
        return false;
    }
    WorkspaceDocument doc = m_store.document();
    // Capture the window's last geometry hint before it goes, purely so a
    // future re-add starts somewhere sensible — the surface itself is
    // deleted below, so this is best-effort.
    if (m_extraCanvases.contains(surfaceId)) {
        evictSurfaceTransiently(surfaceId);
        m_extraCanvases.remove(surfaceId);
    }
    // Destroy UNCONDITIONALLY (red-team #4971 M1): a hidden window is out
    // of m_extraCanvases but its WorkspaceWindow is alive in the host —
    // gating on the map leaked it (and re-minting its surface id later
    // resurrected the corpse).  The host's destroy is a no-op for ids it
    // does not hold.
    if (m_windowHost.destroyWindow) m_windowHost.destroyWindow(surfaceId);
    if (!doc.removeSurface(doc.activeWorkspace, surfaceId)) {
        return false;
    }
    m_store.setDocument(doc);
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    // The orphans moved to the main surface; the forced re-place brings
    // them (and everything else) up in one pass, transient closes undone.
    switchWorkspaceInternal(doc.activeWorkspace, /*force=*/true);
    return true;
}

bool WorkspaceController::renameCanvasWindow(const QString& surfaceId,
                                             const QString& label)
{
    if (!m_enabled) {
        return false;   // consistency with the rest of the window CRUD
    }
    WorkspaceDocument doc = m_store.document();
    if (!doc.renameSurface(doc.activeWorkspace, surfaceId, label)) {
        return false;
    }
    m_store.setDocument(doc);
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    if (const Workspace* ws = m_store.document().workspace(
            m_store.document().activeWorkspace)) {
        if (const WorkspaceSurface* surf = ws->surface(surfaceId)) {
            if (m_windowHost.setWindowLabel) {
                m_windowHost.setWindowLabel(surfaceId, surf->label);
            }
        }
    }
    emit workspacesChanged();
    return true;
}

bool WorkspaceController::setCanvasWindowOpen(const QString& surfaceId, bool open)
{
    if (!m_enabled || surfaceId == WorkspaceSurface::kMainId) {
        return false;
    }
    WorkspaceDocument doc = m_store.document();
    Workspace* ws = nullptr;
    for (Workspace& w : doc.workspaces) {
        if (w.id == doc.activeWorkspace) ws = &w;
    }
    if (!ws) {
        return false;
    }
    WorkspaceSurface* surf = nullptr;
    for (WorkspaceSurface& candidate : ws->surfaces) {
        if (candidate.id == surfaceId) surf = &candidate;
    }
    if (!surf) {
        return false;
    }
    if (surf->hidden == !open) {
        return true;   // the state the caller asked for
    }

    if (!open) {
        // HIDE-AND-KEEP (maintainer ruling): widgets evicted transiently,
        // items untouched, geometry hint captured, window hidden.
        if (m_windowHost.geometryHint) {
            surf->windowGeometry = m_windowHost.geometryHint(surfaceId);
        }
        surf->hidden = true;
        m_store.setDocument(doc);
        if (!m_store.flush()) {
            qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
        }
        evictSurfaceTransiently(surfaceId);
        m_extraCanvases.remove(surfaceId);
        if (m_windowHost.closeWindow) m_windowHost.closeWindow(surfaceId);
    } else {
        surf->hidden = false;
        m_store.setDocument(doc);
        if (!m_store.flush()) {
            qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
        }
        reconcileWindowsWithActiveWorkspace();
        placeSurfaceItems(surfaceId);
    }
    emit workspacesChanged();
    return true;
}

QList<WorkspaceController::CanvasWindowInfo>
WorkspaceController::canvasWindowList() const
{
    QList<CanvasWindowInfo> out;
    const WorkspaceDocument& doc = m_store.document();
    if (const Workspace* ws = doc.workspace(doc.activeWorkspace)) {
        for (const WorkspaceSurface& s : ws->surfaces) {
            if (s.id == WorkspaceSurface::kMainId) continue;
            CanvasWindowInfo info;
            info.id    = s.id;
            info.label = s.label.isEmpty() ? s.id : s.label;
            info.open  = !s.hidden;
            info.live  = (canvasForSurface(s.id) != nullptr);
            out.append(info);
        }
    }
    return out;
}

bool WorkspaceController::moveItemToSurface(const QString& itemId,
                                            const QString& surfaceId)
{
    if (!m_enabled) {
        return false;
    }
    if (itemId == kBandStackItemId || itemId == kPanStackItemId) {
        return false;   // shell furniture stays on the main surface
    }
    WorkspaceDocument doc = m_store.document();
    Workspace* ws = nullptr;
    for (Workspace& w : doc.workspaces) {
        if (w.id == doc.activeWorkspace) ws = &w;
    }
    if (!ws || !ws->surface(surfaceId)) {
        return false;
    }
    const QString from = docSurfaceForItem(*ws, itemId);
    if (from == surfaceId) {
        return true;   // the state the caller asked for
    }

    // DOCUMENT FIRST (the switch-pivot lesson): the item entry moves
    // surfaces atomically, so a crash mid-reparent boots into a document
    // that already says where the item lives.
    CanvasItem moved;
    bool found = false;
    for (WorkspaceSurface& surf : ws->surfaces) {
        for (int i = 0; i < surf.items.size(); ++i) {
            if (surf.items.at(i).id == itemId) {
                moved = surf.items.takeAt(i);
                found = true;
                break;
            }
        }
        if (found) break;
    }
    if (!found) {
        // Untracked but live (a pan placed this session): synthesize from
        // the canvas.
        WorkspaceCanvas* holder = canvasHolding(itemId);
        if (!holder) {
            return false;
        }
        moved.id          = itemId;
        moved.contentType = itemId.startsWith(kPanItemPrefix)
                                ? QStringLiteral("panadapter")
                                : QStringLiteral("applet");
        moved.rect        = holder->itemRect(itemId);
    }
    for (WorkspaceSurface& surf : ws->surfaces) {
        if (surf.id == surfaceId) {
            // Moving something TO a window is asking to see it there.
            surf.hidden = false;
            surf.items.append(moved);
            break;
        }
    }
    m_store.setDocument(doc);
    if (!m_store.flush()) {
        qWarning() << "WorkspaceController: workspace edit did not persist (read-only session?)";
    }
    reconcileWindowsWithActiveWorkspace();

    WorkspaceCanvas* target = canvasForSurface(surfaceId);
    WorkspaceCanvas* source = canvasHolding(itemId);
    if (!target) {
        emit workspacesChanged();
        return true;   // recorded; the window will place it when it opens
    }

    // ── The widget follows, in ONE step ──────────────────────────────────
    // A cross-window move is a cross-TOP-LEVEL reparent — the
    // #2495/#4617/#4319 crash lineage — so pans take the floatPanadapter
    // recipe through the pan-host hooks and applets go through the
    // manager's reparent preparation (returnFromCanvas/detachForCanvas
    // both call prepareRhiChildrenForReparent).
    m_applying = true;
    if (itemId.startsWith(kPanItemPrefix)) {
        const QString panId = panIdForItem(itemId);
        if (!panId.isEmpty()) {
            if (m_panHost.prepareTopLevelMove) {
                m_panHost.prepareTopLevelMove(panId);
            }
            QWidget* w = source ? source->releaseItem(itemId) : nullptr;
            if (!w && m_panHost.detach) {
                w = m_panHost.detach(panId);   // e.g. still in the stack
            }
            if (w) {
                target->addItem(itemId, w, moved.rect,
                                QStringLiteral("panadapter"), QSize(320, 180));
                target->sendItemToBack(itemId);
            }
            if (m_panHost.finishTopLevelMove) {
                m_panHost.finishTopLevelMove(panId);
            }
        }
    } else if (itemId.startsWith(kAppletItemPrefix)) {
        ContainerWidget* c =
            containerForApplet(itemId.mid(kAppletItemPrefix.size()));
        if (c) {
            if (c->isOnCanvas() && source) {
                source->takeItem(itemId);
                m_manager->returnFromCanvas(c->id(), c);
            }
            // Coming OFF a hidden window: hide-and-keep left the applet
            // transiently closed and on no canvas.  The move to a live
            // surface is the operator asking to SEE it — reopen under the
            // guard, exactly as placeSurfaceItems does on window reopen
            // (red-team #4971 M4: the document moved, nothing appeared,
            // and the divergence popped the applet open at the next full
            // recall with no user action).
            if (!moved.closed && !c->isContainerVisible() && !c->isFloating()) {
                if (m_panHost.recallGuard) m_panHost.recallGuard(true);
                c->setContainerVisible(true);
                if (m_panHost.recallGuard) m_panHost.recallGuard(false);
            }
            if (c->isContainerVisible() && !c->isFloating()
                && m_manager->detachForCanvas(c->id()) == c) {
                target->addItem(itemId, c, moved.rect,
                                QStringLiteral("applet"));
            }
        }
    }
    m_applying = false;

    emit workspacesChanged();
    return true;
}

void WorkspaceController::noteWindowGeometryHint(const QString& surfaceId,
                                                 const QByteArray& hint)
{
    WorkspaceDocument doc = m_store.document();
    for (Workspace& ws : doc.workspaces) {
        if (ws.id != doc.activeWorkspace) continue;
        for (WorkspaceSurface& s : ws.surfaces) {
            if (s.id != surfaceId) continue;
            if (s.windowGeometry == hint) return;
            s.windowGeometry = hint;
            m_store.setDocument(doc);   // touch — the store debounces
            return;
        }
    }
}

void WorkspaceController::prepareShutdown()
{
    // BEFORE the pan/container teardown (MainWindow's ordered shutdown):
    // capture every open window's geometry hint, flush the document once,
    // then destroy the windows — explicitly, the #2495 lesson: a floating
    // top-level left to ~QWidget cleanup crashed on macOS at exit.
    const QStringList openWindows = m_extraCanvases.keys();
    for (const QString& sid : openWindows) {
        if (m_windowHost.geometryHint) {
            noteWindowGeometryHint(sid, m_windowHost.geometryHint(sid));
        }
    }
    m_store.flush();
    for (const QString& sid : openWindows) {
        // Evict BEFORE destroying: the stack still owns every pan applet
        // and the panel every container — a window deleted around them
        // would take them along (they are its children at this point).
        evictSurfaceTransiently(sid);
        m_extraCanvases.remove(sid);
        if (m_windowHost.destroyWindow) m_windowHost.destroyWindow(sid);
    }
    // Hidden windows are NOT in m_extraCanvases and their widgets are
    // already evicted — the HOST sweeps its whole window map after this
    // (red-team #4971 M1: disable-then-quit left every canvas window to
    // ~QWidget, the exact #2495 shape this method exists to prevent).
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

    // The OLD workspace's window geometry is captured before the pivot —
    // the hint writes must land on the workspace that owns those windows,
    // not the one we are switching to (red-team #4971 L2).
    for (auto it = m_extraCanvases.constBegin();
         it != m_extraCanvases.constEnd(); ++it) {
        if (m_windowHost.geometryHint) {
            noteWindowGeometryHint(it.key(), m_windowHost.geometryHint(it.key()));
        }
    }
    doc = m_store.document();   // the hint writes made the local copy stale

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
    // Windows first (phase 7): close the ones the target does not want,
    // open the ones it does — placement below needs the canvases to
    // exist.  The widgets were all released above.  Hints re-apply: each
    // workspace owns its windows' geometry (L2).
    reconcileWindowsWithActiveWorkspace(/*reapplyHints=*/true);

    QSet<QString> wanted;
    if (const Workspace* ws = doc.workspace(id)) {
        for (const WorkspaceSurface& surf : ws->surfaces) {
            if (surf.hidden) {
                continue;   // hide-and-keep: a closed window's applets
                            // stay transiently shut until it reopens
            }
            for (const CanvasItem& it : surf.items) {
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
                                               const QPointF& canvasPos,
                                               const QString& surfaceId)
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

    // Refusals come BEFORE any write (red-team #4971 B2: a refused add
    // had already committed writeItemPresence, mangling a hidden
    // window's stored rect with coordinates from a different canvas).
    // With no explicit surface, the add lands wherever the item lives —
    // and a hidden home means there is nothing to place on: refuse as a
    // genuine no-op.  An explicit surface is intent and relocates, so a
    // hidden home is no obstacle — but a hidden TARGET is.
    {
        const WorkspaceDocument& doc = m_store.document();
        const Workspace* ws = doc.workspace(doc.activeWorkspace);
        if (!ws) {
            return false;
        }
        if (!surfaceId.isEmpty()) {
            const WorkspaceSurface* target = ws->surface(surfaceId);
            if (!target) {
                // A typo'd or stale surface must be an ERROR, exactly as
                // it is for moveItemToSurface — not a silent landing on
                // main (red-team #4971 N4).
                return false;
            }
            if (target->hidden) {
                // An EXPLICIT hidden target is the operator asking to see
                // it there — the same rule moveItemToSurface states.
                // Reopening also re-places the window's existing items,
                // so the add below lands on a live canvas.
                if (!setCanvasWindowOpen(surfaceId, true)) {
                    return false;
                }
            }
        } else {
            const QString home = docSurfaceForItem(*ws, itemId);
            if (!home.isEmpty()) {
                if (const WorkspaceSurface* surf = ws->surface(home)) {
                    if (surf->hidden) {
                        return false;   // implicit add: its window is closed
                    }
                }
            }
        }
    }

    const NormRect rect  = defaultRectFor(c, &canvasPos,
                                          canvasForSurface(surfaceId));

    // Whatever state the applet is in, the palette add ends the same way:
    // an OPEN applet placed at the click point ON THE CANVAS THE MENU WAS
    // OPENED OVER (phase 7).  The rect is recorded first so every path
    // below places from the document — the same one-placement-path shape
    // import-floats uses — and recording carries the surface (an explicit
    // surface relocates a previous home, red-team #4971 B1).
    writeItemPresence(itemId, QStringLiteral("applet"), rect,
                      /*present=*/true, /*flushNow=*/false, surfaceId);

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
        // Explicit main (red-team #4971 L1): the rects are mapped against
        // the MAIN canvas, and an applet whose item lived on an extra
        // window would otherwise import onto that window at main-mapped
        // coordinates.  Import is main-surface by design; the explicit
        // surface relocates a stray home.
        writeItemPresence(itemIdFor(c), QStringLiteral("applet"), mapped,
                          /*present=*/true, /*flushNow=*/false,
                          WorkspaceSurface::kMainId);
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
                              /*present=*/true, /*flushNow=*/false,
                              WorkspaceSurface::kMainId);
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
                                             const QPointF* center,
                                             const WorkspaceCanvas* canvas) const
{
    // Size from the widget's own hint, normalized against the canvas — so a
    // dropped applet arrives at roughly its panel size instead of a house
    // number.  The canvas clamps against its minimum floor either way.
    const QSize canvasSize = (canvas ? canvas : m_canvas)->size();
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
