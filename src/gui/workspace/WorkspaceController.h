#pragma once

// The brain of canvas mode (RFC #4887 phase 3): the one object that knows the
// store, the canvas and the container manager at the same time, and therefore
// the only place placement POLICY lives.  The canvas stays a dumb surface,
// the manager stays a reparenting mechanism, the store stays persistence —
// what a drop means, when an applet belongs on the canvas, and what the
// document says about any of it is decided here and nowhere else.
//
// This is deliberately a real class rather than a fistful of MainWindow
// members — the #3557 direction: MainWindow mounts the canvas and forwards
// the View-menu toggle, and everything else lives behind this seam.
//
// ── The membership rule ──────────────────────────────────────────────────
//
// A document item "applet:<id>" means "this applet BELONGS on the canvas".
// The applet is actually PLACED when canvas mode is on, the applet is open,
// and it is not floating (pop-out stays, RFC decision 1).  Consequences:
//
//   * closing an applet (bar button) evicts it but KEEPS its item, so
//     reopening returns it to the spot it had;
//   * leaving the canvas deliberately — the title-bar "return to panel"
//     button, dragging it onto the panel, or popping it out — REMOVES the
//     item: the operator said "not on the canvas", and a home that silently
//     reasserts itself is how layouts stop being trusted;
//   * disable() keeps every item, because switching the mode off is not a
//     statement about any applet.
//
// The document is placement truth (Principle V).  ContainerTree's
// mode:"canvas" string is a dual-write mirror, and ContainerManager::
// restoreState() deliberately normalises it back to panel at startup — this
// controller re-places from the document when the mode comes up.

#include "gui/workspace/WorkspaceStore.h"

#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QString>
#include <QStringList>

class QWidget;

namespace AetherSDR {

class ContainerManager;
class ContainerWidget;
class WorkspaceCanvas;

class WorkspaceController : public QObject {
    Q_OBJECT

public:
    // The reserved item holding the panadapter area in phase 3.  Phase 4
    // splits it into per-pan items; until then it is one non-closable region
    // whose rect the document carries like any other item's.
    static const QString kPanStackItemId;

    WorkspaceController(ContainerManager* manager,
                        WorkspaceCanvas* canvas,
                        QObject* parent = nullptr);

    // Startup: read the stored document (never migrates — migration is an
    // explicit consequence of the operator enabling the mode).  Returns true
    // when a usable document asks for canvas mode, i.e. the caller should
    // mount the canvas and call enable().
    bool boot();

    // Turn the mode on.  The canvas must already be mounted and
    // setPanStackWidget() called.  First enable migrates the legacy layout
    // keys into a Classic document (`knownAppletIds` scopes that read).
    // False + `whyNot` when the store refuses — including the write-blocked
    // newer-document case, which must surface rather than silently shrug.
    bool enable(const QStringList& knownAppletIds, QString* whyNot = nullptr);

    // Turn the mode off: every applet returns to its panel slot, the pan
    // stack widget is released (parentless — the caller re-slots it), and
    // the document keeps all placement for the next enable().
    void disable();

    bool isEnabled() const { return m_enabled; }

    // The widget occupying the reserved pan area (the PanadapterStack).
    // Held, not owned; enable() places it, disable() releases it.
    void setPanStackWidget(QWidget* w);

    // ── Applet movement ──────────────────────────────────────────────────
    //
    // `appletId` is the panel's entry id (== the container's dragId(); the
    // container's own id may differ, e.g. TXDSP wraps "tx_dsp").
    //
    // Send: detach from the panel (docking first if floating) and place at
    // `where` when given, else at the document's remembered rect, else at a
    // default.  Return: back to the panel slot it came from, forgetting the
    // canvas home.  Both are no-ops outside canvas mode.
    bool sendAppletToCanvas(const QString& appletId,
                            const NormRect* where = nullptr);
    bool returnAppletToPanel(const QString& appletId);

    // ── Escape hatches (RFC #4887 phase 5) ───────────────────────────────
    //
    // Undo restores the rect the last gesture (mouse or keyboard) started
    // from — single-slot by design, per the RFC's "undo for the last
    // placement"; invoking it twice toggles, which doubles as redo.
    bool undoLastPlacement();
    bool canUndo() const { return !m_undoItemId.isEmpty(); }

    // Rebuild the active workspace's main surface as Classic — pan area plus
    // the open applets in a fresh column — and re-place everything.  The one
    // guaranteed way back to a sane shell.
    void resetToClassic();

    // Resolve applet-vs-applet overlaps by minimal downward pushes.  Items
    // overlapping the PAN AREA are left alone on purpose: a meter over the
    // spectrum is a feature, not disorder.
    void tidyLayout();

    // Where a live drag out of the canvas may land (the applet panel).
    // Releasing a move over this widget returns the applet to it; anywhere
    // else the drag is an abort (the canvas has already restored the rect).
    void setReturnTarget(QWidget* target);

signals:
    void enabledChanged(bool enabled);

private:
    ContainerWidget* containerForApplet(const QString& appletId) const;
    QString appletIdFor(const ContainerWidget* c) const;
    QString itemIdFor(const ContainerWidget* c) const;

    // Take the container off the canvas and back to its panel slot.
    // `forgetHome` distinguishes "the operator left the canvas" (remove the
    // document item) from "the applet closed" (keep it for reopening).
    void evictFromCanvas(ContainerWidget* c, bool forgetHome);

    // Document edits.  Each takes the current document, applies one change,
    // and hands it back to the store; `flushNow` marks the end of a gesture.
    void writeItemRect(const QString& itemId, const NormRect& rect, bool flushNow);
    void writeItemPresence(const QString& itemId, const QString& contentType,
                           const NormRect& rect, bool present, bool flushNow);
    void writeStackingFromCanvas();

    void onDropReceived(const QString& payload, const QPointF& pos);
    void onItemRectChanged(const QString& itemId, const NormRect& rect);
    void onContainerCreated(const QString& containerId);
    void wireContainer(ContainerWidget* c);
    void onItemDraggedOut(const QString& itemId, const QPoint& globalPos);
    void onContextMenuRequested(const QString& itemId, const QPoint& globalPos);

    // The placement replay shared by enable() and resetToClassic(): put the
    // pan stack and every eligible applet item of the active workspace onto
    // the (empty) canvas.  Callers hold m_applying.
    void placeActiveWorkspaceItems(WorkspaceDocument& doc, bool* docChanged);

    NormRect defaultRectFor(const ContainerWidget* c, const QPointF* center) const;
    NormRect panStackRectFromDocument() const;

    ContainerManager* m_manager{nullptr};
    WorkspaceCanvas*  m_canvas{nullptr};
    WorkspaceStore    m_store;
    QPointer<QWidget> m_panStackWidget;
    QPointer<QWidget> m_returnTarget;
    QStringList m_knownAppletIds;   // from enable(), for resetToClassic()
    QString  m_undoItemId;      // last gesture's item…
    NormRect m_undoRect;        // …and the rect it started from
    bool m_enabled{false};
    // True while enable()/disable() replay the document onto the canvas —
    // the canvas signals fired by that replay describe what the document
    // already says, and echoing them back into it would be the #4427
    // write-back-what-you-read mistake.
    bool m_applying{false};
};

}  // namespace AetherSDR
