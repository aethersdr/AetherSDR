#pragma once

#include "ContainerWidget.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

namespace AetherSDR {

class FloatingContainerWindow;

// Coordinates the lifecycle of ContainerWidget instances — creation,
// destruction, float/dock transitions, and serialisation to AppSettings.
//
// Phase 2 scope: flat registry (no nesting yet).  Each container is
// owned by the manager and may have a single parent QWidget that it
// returns to when docked.  A content-factory registry lets persisted
// state rehydrate leaf content on restore.
class ContainerManager : public QObject {
    Q_OBJECT

public:
    using ContentFactory =
        std::function<QWidget*(const QString& containerId)>;

    explicit ContainerManager(QObject* parent = nullptr);
    ~ContainerManager() override;

    // ── Content factory registry ─────────────────────────────────
    //
    // Each leaf content type registers a factory keyed by a short
    // string (e.g. "ClientChainApplet").  On restore the manager
    // reads the stored `contentType` for each container and calls
    // the matching factory to rematerialize the content widget.
    void registerContent(const QString& typeId, ContentFactory factory);

    // ── Container lifecycle ──────────────────────────────────────
    //
    // Creates a new ContainerWidget, registers it under `id`, and
    // returns it.  `contentType` (optional) is persisted so the
    // matching factory can rebuild content on restore.  `parentId`
    // (optional) nests this container inside another: the new
    // container is inserted as a child of `parentId`'s body layout
    // at `index` (-1 = append).  Top-level containers pass empty
    // parentId and place themselves into an external layout
    // manually.
    ContainerWidget* createContainer(const QString& id,
                                     const QString& title,
                                     const QString& contentType = {},
                                     const QString& parentId = {},
                                     int index = -1);

    // Destroy a container by ID.  Recursively destroys any child
    // containers first so floating-window cleanup proceeds children-
    // before-parent.  Removes from registry, deletes the widget.
    void destroyContainer(const QString& id);

    // ── Parent / child queries ───────────────────────────────────
    QString parentOf(const QString& id) const;
    QStringList childrenOf(const QString& id) const;

    // Reparent a container: remove from current parent's body, insert
    // into new parent's body at `index` (or at the current outer
    // layout if `newParentId` is empty — makes it top-level again).
    // Handles floating children correctly: they stay in their own
    // windows; only the reparented container's logical parent changes.
    void reparentContainer(const QString& id,
                            const QString& newParentId,
                            int index = -1);

    // ── Queries ──────────────────────────────────────────────────
    ContainerWidget* container(const QString& id) const;
    QList<ContainerWidget*> allContainers() const;
    int containerCount() const;

    // ── Dock transitions ─────────────────────────────────────────
    //
    // Float: detach from current parent layout, hand to a fresh
    // FloatingContainerWindow, remember original parent.
    // Dock: close window, reparent back to original.
    void floatContainer(const QString& id);
    void dockContainer(const QString& id);

    // ── Canvas transitions (RFC #4887 phase 3) ───────────────────
    //
    // The third placement: the container becomes a child of a
    // WorkspaceCanvas, which owns its geometry from then on.
    //
    // These mirror float/dock deliberately — same detach-and-remember
    // step, same #2495 RHI guard, same restore-to-the-original-slot on
    // the way back.  The failure modes are identical, and a second,
    // subtly different reparent path is how the float/dock crash
    // lineage (#2495, #4319, #4617) got as long as it did.
    //
    // detachForCanvas() takes the container out of its panel slot and
    // hands it back for the caller to place — the canvas needs a rect,
    // which is workspace state this class does not own.  Returns
    // nullptr for an unknown id or one already on a canvas.  A
    // FLOATING container is docked first, so "float, then canvas" is
    // not a special case anyone has to remember.
    ContainerWidget* detachForCanvas(const QString& id);

    // Put it back in the slot it left, exactly as dockContainer() does
    // after a float.  The caller has already taken it off the canvas
    // (WorkspaceCanvas::takeItem()).
    void returnFromCanvas(const QString& id, ContainerWidget* c);

    // The manager's one hook into whoever owns the canvas.  When a
    // canvas-mode container asks to dock (title-bar button) or must leave
    // the canvas before floating, the manager calls this to have the
    // controller take the item off the canvas and hand the container back
    // through returnFromCanvas().  Kept as a callback rather than a
    // signal because the transition must complete synchronously — a
    // float that queued the eviction would reparent a widget the canvas
    // still owns.
    void setCanvasEvictor(std::function<void(const QString&)> evictor);

    // Follow the main-window frameless setting for all active floating windows.
    void setFramelessMode(bool on);

    // Save geometry and close all floating windows without triggering
    // dock-back behaviour.  Call from MainWindow::closeEvent().
    void prepareShutdown();

    // ── Persistence ──────────────────────────────────────────────
    //
    // State is stored as JSON under the AppSettings key
    // `ContainerTree`.  Schema (Phase 2 — flat):
    //   {
    //     "version": 1,
    //     "containers": {
    //       "<id>": {
    //         "mode": "panel" | "floating",
    //         "visible": true | false,
    //         "contentType": "<factory key>",
    //         "geometry": "<base64>"     // only when floating
    //       }
    //     }
    //   }
    //
    // Phase 3 adds "parent" and "children" fields for nesting.
    void saveState() const;
    void restoreState();

signals:
    void containerCreated(const QString& id);
    void containerDestroyed(const QString& id);

private slots:
    void onFloatRequested();
    void onDockRequested();
    void onCloseRequested();
    void onAlwaysOnTopToggled(bool on);
    void onFloatingWindowDock(ContainerWidget* c);

private:
    struct Meta {
        QString     contentType;
        QString     parentId;               // logical container parent ("" = top-level)
        QWidget*    originalParent{nullptr};// non-container layout parent (top-level case)
        int         originalIndex{-1};      // remembered slot for re-dock
    };

    void wireContainer(ContainerWidget* container);

    // The two halves every placement transition shares: remember and leave
    // the current slot, and return to the remembered one.  Factored out when
    // the canvas became a third placement — float/dock and canvas/panel are
    // the same reparent with a different destination, and keeping one
    // implementation is what stops them drifting apart.
    void detachFromCurrentSlot(const QString& id, ContainerWidget* c);
    void restoreToOriginalSlot(const QString& id, ContainerWidget* c);

    QMap<QString, QPointer<ContainerWidget>> m_containers;
    QMap<QString, FloatingContainerWindow*> m_floatingWindows;
    QMap<QString, ContentFactory>           m_factories;
    std::function<void(const QString&)>     m_canvasEvictor;
    QMap<QString, Meta>                     m_meta;

    // True only while restoreState() is replaying saved state, so saveState()
    // suppresses its durable flush during restore (it would just re-write what
    // it is reading). User-gesture transitions always flush (#4427).
    bool m_restoring{false};
};

} // namespace AetherSDR
