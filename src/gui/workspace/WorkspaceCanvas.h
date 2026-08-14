#pragma once

// The workspace canvas surface (RFC #4887, phase 1).
//
// A QWidget that hosts freely-placed child widgets, positioned from a
// CanvasLayout.  Deliberately thin: every decision that can be got wrong
// (clamping, hit testing, stacking) lives in the widget-free model, and this
// class only applies the answers to real geometry.  If a change here needs a
// new rule rather than a new call, the rule belongs in CanvasLayout where it
// can be tested headless.
//
// PHASE 1 SCOPE — what is deliberately absent, and where it lands:
//   * snapping, alignment guides, resize handles, tidy, keyboard nudge — phase 5
//   * persistence and the workspace document                          — phase 2
//   * mounting into MainWindow, and any theme tokens for item chrome   — phase 3
//   * automation bridge verbs                                          — phase 4
//
// Nothing in this file touches MainWindow, TitleBar, AutomationServer or the
// theme seed: those are RFC #4764's files, and phase 1 runs in parallel with
// it precisely because it stays out of them.

#include "gui/workspace/CanvasInteraction.h"
#include "gui/workspace/CanvasLayout.h"

#include <QHash>
#include <QPointF>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QWidget>

namespace AetherSDR {

class WorkspaceCanvas : public QWidget {
    Q_OBJECT

public:
    explicit WorkspaceCanvas(QWidget* parent = nullptr);

    // Drops the per-item destroyed-watches.  Not optional: ~QWidget deletes
    // the canvas's children AFTER this class's members are gone, so a watch
    // left connected would fire into a lambda touching a destroyed
    // CanvasLayout. See the destructor.
    ~WorkspaceCanvas() override;

    // Place `content` on the canvas at `rect`.  The canvas reparents it and
    // owns its geometry from then on — callers must not setGeometry() it
    // afterwards, or the next resize will silently undo them.
    //
    // Returns false for an empty or duplicate id, or a null widget; on
    // failure `content` is left entirely alone, not deleted, so a caller that
    // hit a duplicate id still owns something it can place elsewhere.
    bool addItem(const QString& id,
                 QWidget* content,
                 const NormRect& rect,
                 const QString& contentType = {},
                 const QSize& minimumSize = QSize(160, 90));

    // Rehydrate a saved surface: place `widgets` (keyed by item id) according
    // to `items`, honouring their stored z.
    //
    // This exists because addItem() deliberately ignores a caller's z, and
    // WorkspaceCanvas is the ONLY public way onto a canvas — so without it a
    // restore through the widget layer would scramble stacking unless it
    // happened to feed items in ascending z, which nothing made true
    // (PR #4900 review, M3).  Items with no matching widget are skipped.
    // Returns how many were placed.
    int restoreItems(const QList<CanvasItem>& items,
                     const QHash<QString, QWidget*>& widgets);

    // Removes the item and deletes its widget.  Use takeItem() to keep it.
    bool removeItem(const QString& id);

    // Removes the item and hands the widget back, reparented to nullptr and
    // hidden.  This is the call phase 3 needs to move an applet between a
    // canvas and a container without destroying it.
    QWidget* takeItem(const QString& id);

    // Drops the canvas's claim on the item — entry, selection, gesture,
    // filter, destroyed-watch — WITHOUT touching the widget's parent.  The
    // pan-item path (#4887 phase 4): a pan that floats has already been
    // adopted by its PanFloatingWindow, and the detour takeItem() makes
    // through setParent(nullptr) is exactly the transient-top-level move the
    // #1344 lineage forbids for QRhi children.  Callers own the reparent.
    QWidget* releaseItem(const QString& id);

    QWidget* itemWidget(const QString& id) const;
    bool contains(const QString& id) const { return m_layout.contains(id); }
    int itemCount() const { return m_layout.count(); }

    // Placement.  The rect is clamped against the current canvas size, so the
    // value read back may differ from the one passed in.
    bool setItemRect(const QString& id, const NormRect& rect);
    NormRect itemRect(const QString& id) const;

    // Item under a point in this widget's coordinates; empty over bare canvas.
    QString hitTest(const QPoint& pos) const;

    bool raiseItem(const QString& id);
    bool lowerItem(const QString& id);
    bool bringItemToFront(const QString& id);
    bool sendItemToBack(const QString& id);

    // Read-only view of the model, for tests and for phase 2's serializer.
    const CanvasLayout& layout() const { return m_layout; }

    // ── Selection + interactive gestures (RFC #4887 phase 5) ─────────────
    //
    // One item may be selected; selection shows the grip frame (resize) and
    // is what the keyboard operates on.  Pressing an item selects and
    // raises it; pressing bare canvas clears.
    void selectItem(const QString& id);
    void clearSelection();
    QString selectedItem() const { return m_selectedId; }

    // The gesture session — one at a time, driven from three sources that
    // all land here: the title bar's live-move stream (via the controller),
    // the grip frame's resize drags, and the keyboard.  The session applies
    // applyDrag()+snapRect() live, so the item follows the cursor with snap
    // guides painted; Esc cancels (restores the start rect); holding Alt
    // suppresses snapping for the duration of that motion.
    void beginMoveGesture(const QString& id, const QPoint& globalPos);
    void beginFrameGesture(HitZone zone, const QPoint& globalPos);  // on the selection
    void moveGesture(const QPoint& globalPos);
    void endGesture(const QPoint& globalPos);
    void cancelGesture();
    bool gestureActive() const { return !m_gestureId.isEmpty(); }

    // Snap capture distance (px on the live canvas).
    static constexpr int kSnapTolerancePx = 8;
    // The snap grid, in NORMALIZED divisions — field-tuned to 96x54
    // (~20 px cells at 1920, exactly square at 16:9).  Normalized on
    // purpose: a pixel grid would put an item on-grid at one window size and
    // off-grid at every other, which is the exact failure the fractional
    // model exists to prevent.  1 px dots mark the intersections: always in
    // the canvas background, and on top of everything while a gesture is
    // live (the only time the targets matter more than the content).
    static constexpr int kSnapGridColumns = 96;
    static constexpr int kSnapGridRows    = 54;
    // The grid's own magnet, gentler than the peer tier's: at ~20 px pitch a
    // full 8 px pull would capture nearly every position and free placement
    // would need Alt held permanently.
    static constexpr int kGridSnapTolerancePx = 4;

    // Whether gestures snap to the grid (the dots stay either way) — the
    // RFC's "optional grid", toggled from the canvas context menu.
    void setGridSnapEnabled(bool on) { m_gridSnapEnabled = on; }
    bool isGridSnapEnabled() const { return m_gridSnapEnabled; }

    // ── Edit mode (8600 field request) ───────────────────────────────────
    //
    // A locked canvas is for OPERATING: no click-to-select, no frame, no
    // title-strip drags, no drops, no nudge keys, no grid dots — presses go
    // to the applet content and nothing arms placement.  Edit mode is the
    // arranging posture: everything above comes back.  The BARE widget
    // defaults to editing (a canvas with no controller is an editor — the
    // phase-1 tests and any future preview use it that way); the
    // controller imposes the locked operating posture at enable().
    //
    // Deliberately NOT gated here: setItemRect/restoreItems (lifecycle
    // placement — reopen-restore, pan arrival, dock-return and the
    // automation bridge are not operator edits), and the context-menu
    // signal (the controller builds a mode-aware menu).
    void setEditMode(bool on);
    bool isEditMode() const { return m_editMode; }
    // Keyboard nudge step (px); Ctrl divides it for fine placement.
    static constexpr int kNudgePx = 8;

    // ── Drag-and-drop (RFC #4887 phase 3) ────────────────────────────────
    //
    // The canvas accepts drops of one MIME type and reports them upward as
    // (payload, normalized point); it applies no policy of its own.  What a
    // drop MEANS — place a panel applet, move an existing item, refuse —
    // is the WorkspaceController's call, because the answer depends on
    // state the canvas cannot see (the panel, the manager, the document).
    // Empty MIME type (the default) leaves drops disabled.
    void setDropMimeType(const QByteArray& mimeType);
    QByteArray dropMimeType() const { return m_dropMimeType; }

signals:
    // A drop of the accepted MIME type landed at `pos` (canvas fractions).
    // `payload` is the MIME data verbatim — for the applet MIME this is the
    // container's dragId().
    void dropReceived(const QString& payload, const QPointF& pos);

    // Selection, for the frame, the controller's context menu, and a11y.
    void selectionChanged(const QString& id);   // empty = cleared

    // Edit mode flipped — the View-menu action and the context menu both
    // drive it, so each keeps the other honest through this.
    void editModeChanged(bool on);

    // A gesture began (undo snapshots hang off this) and committed.  A
    // cancelled gesture emits neither finish nor a rect change beyond the
    // restore.  Keyboard nudges emit the same pair, so every placement
    // change flows through one seam.
    void gestureStarted(const QString& id, const NormRect& startRect);
    void gestureFinished(const QString& id);

    // A live move was RELEASED outside the canvas.  The item has already
    // been restored to its start rect — whoever owns the surroundings
    // decides whether the release point means something (the controller
    // returns applets dropped onto the panel).
    void itemDraggedOut(const QString& id, const QPoint& globalPos);

    // Context menu request: `id` is the item under the cursor, empty over
    // bare canvas.  Policy lives with the controller.
    void contextMenuRequested(const QString& id, const QPoint& globalPos);

    // Emitted when an item's STORED rect changes — placement gestures only.
    // A canvas resize emits nothing: stored rects are canvas-independent,
    // and the squeeze a small window forces on the view is display-time
    // compromise (applyGeometryFor), never an edit.  Growing the window
    // therefore restores the arrangement exactly, and a transient size at
    // startup can no longer rewrite the document (the phase 3 field report).
    //
    // Both of these are edits: phase 2's auto-commit turns each into a
    // whole-document write, so neither fires for an operation that changed
    // nothing — raising an already-frontmost item is silent too.
    void itemRectChanged(const QString& id, const NormRect& rect);
    void itemStackingChanged(const QString& id);
    void itemAdded(const QString& id);
    void itemRemoved(const QString& id);

protected:
    bool event(QEvent* ev) override;   // ShortcutOverride: placement keys win
    void resizeEvent(QResizeEvent* ev) override;
    void dragEnterEvent(QDragEnterEvent* ev) override;
    void dragMoveEvent(QDragMoveEvent* ev) override;
    void dropEvent(QDropEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;   // bare canvas: deselect
    void keyPressEvent(QKeyEvent* ev) override;
    void paintEvent(QPaintEvent* ev) override;        // snap guides
    void contextMenuEvent(QContextMenuEvent* ev) override;

    // Raises an item when its widget is pressed.  Installed on the item widget
    // itself, so a press landing on a deeper descendant does not raise — real
    // content drives that from its own title bar once containers arrive in
    // phase 3, rather than this class filtering the whole application.
    bool eventFilter(QObject* watched, QEvent* ev) override;

private:
    // Model -> pixels, for every item.
    void applyGeometry();

    // The display rect of one item — the stored rect through the display
    // clamp.  What applyGeometryFor() sets and what the frame follows.
    QRect displayRectFor(const QString& id) const;

    void beginGesture(const QString& id, HitZone zone, const QPoint& globalPos);
    void updateFrame();
    QSizeF minNormFor(const QString& id) const;

    // Model -> Qt stacking.  Raising bottom-to-top leaves the highest z on
    // top; Qt has no "set stacking index", so the order of these calls IS the
    // result.
    void applyStacking();

    void applyGeometryFor(const QString& id);

    CanvasLayout m_layout;
    QHash<QString, QPointer<QWidget>> m_widgets;
    QByteArray m_dropMimeType;

    // Selection + frame (phase 5).
    QString m_selectedId;
    class CanvasItemFrame* m_frame{nullptr};
    // Gesture visuals — guides + grid dots painted ABOVE the items (a
    // widget's own paint sits under its children, so the canvas paintEvent
    // can only reach exposed background).  Mouse-transparent.
    class GestureOverlay;
    GestureOverlay* m_gestureOverlay{nullptr};

    // Active gesture.
    bool m_gridSnapEnabled{true};
    bool m_editMode{true};
    QString  m_gestureId;
    HitZone  m_gestureZone{HitZone::None};
    NormRect m_gestureStart;
    QPoint   m_gestureOrigin;   // global press position
    QList<double> m_vGuides;
    QList<double> m_hGuides;
};

}  // namespace AetherSDR
