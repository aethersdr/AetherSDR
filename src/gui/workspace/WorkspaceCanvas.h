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

#include "gui/workspace/CanvasLayout.h"

#include <QHash>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QWidget>

namespace AetherSDR {

class WorkspaceCanvas : public QWidget {
    Q_OBJECT

public:
    explicit WorkspaceCanvas(QWidget* parent = nullptr);

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

    // Removes the item and deletes its widget.  Use takeItem() to keep it.
    bool removeItem(const QString& id);

    // Removes the item and hands the widget back, reparented to nullptr and
    // hidden.  This is the call phase 3 needs to move an applet between a
    // canvas and a container without destroying it.
    QWidget* takeItem(const QString& id);

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

signals:
    // Emitted whenever an item's stored rect changes — including the clamps
    // applied on a canvas resize, which is why the rect is carried in the
    // signal rather than left for the receiver to read back.
    void itemRectChanged(const QString& id, const NormRect& rect);
    void itemStackingChanged(const QString& id);
    void itemAdded(const QString& id);
    void itemRemoved(const QString& id);

protected:
    void resizeEvent(QResizeEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;

    // Raises an item when its widget is pressed.  Installed on the item widget
    // itself, so a press landing on a deeper descendant does not raise — real
    // content drives that from its own title bar once containers arrive in
    // phase 3, rather than this class filtering the whole application.
    bool eventFilter(QObject* watched, QEvent* ev) override;

private:
    // Model -> pixels, for every item.
    void applyGeometry();

    // Model -> Qt stacking.  Raising bottom-to-top leaves the highest z on
    // top; Qt has no "set stacking index", so the order of these calls IS the
    // result.
    void applyStacking();

    void applyGeometryFor(const QString& id);

    CanvasLayout m_layout;
    QHash<QString, QPointer<QWidget>> m_widgets;
};

}  // namespace AetherSDR
