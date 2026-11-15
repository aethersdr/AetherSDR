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

    // Creates and registers a ContainerWidget under `id`. `contentType` is
    // persisted so its factory can rebuild content on restore. A non-empty
    // `parentId` inserts it into that container's body at `index` (-1 = append);
    // top-level containers are placed into an external layout by the caller.
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

    // Canvas placement (RFC #4887): the container becomes a WorkspaceCanvas child
    // and the canvas owns its geometry. Mirrors float/dock on purpose (same
    // detach-and-remember step, #2495 RHI guard, restore to the original slot); a
    // second reparent path is how the float/dock crashes (#2495, #4319, #4617) grew.
    // detachForCanvas() returns the container for the caller to place (nullptr if
    // unknown or already on a canvas); a floating container is docked first.
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

    // Persistence: JSON under AppSettings `ContainerTree`, version 1:
    //   { "version": 1, "containers": { "<id>": { "mode": "panel"|"floating"|"canvas",
    //     "visible": bool, "contentType": "<factory key>", "parent": "<id>",
    //     "children": ["<id>", ...] } } }
    // A version mismatch skips restore. Floating geometry is stored separately under
    // geometryKeyFor(id).
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
