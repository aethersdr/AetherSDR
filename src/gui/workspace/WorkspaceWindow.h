#pragma once

// One additional canvas window (RFC #4887 phase 7): a thin top-level that
// hosts one WorkspaceCanvas and remembers its own geometry HINT.  The canvas
// does the work; this class holds it — deliberately following
// FloatingContainerWindow's existing frameless/shutdown contract (flags
// re-applied before show, WA_DeleteOnClose off, screen-clamped restore,
// debounced geometry save, an explicit prepareShutdown()) rather than
// inventing a new one, because that contract is where the #2495/#4803
// top-level-window lessons already live.
//
// Geometry is a HINT by schema design (WorkspaceDocument.h): item rects are
// authoritative fractions of this window's canvas, restored exactly; where
// the desktop puts the window is advisory, and Wayland always declines the
// position half.  A hint that is ignored costs one drag and loses no layout.
//
// Lifecycle is the controller's call, made through MainWindow's window-host
// hooks: closing the window is HIDE-AND-KEEP (maintainer ruling) — the
// closeEvent is forwarded as a request, never acted on locally, so the
// controller can evict the surface's widgets first and record the hidden
// flag.  Only prepareShutdown() closes for real.

#include <QByteArray>
#include <QString>
#include <QTimer>
#include <QWidget>

class QCloseEvent;
class QVBoxLayout;

namespace AetherSDR {

class FramelessWindowTitleBar;
class WorkspaceCanvas;

class WorkspaceWindow : public QWidget {
    Q_OBJECT

public:
    // `surfaceId` names the WorkspaceSurface this window realises.  Parent
    // with mainWindow->window(), the FloatingContainerWindow pattern: the
    // window rides above the shell without WindowStaysOnTopHint.
    explicit WorkspaceWindow(const QString& surfaceId, QWidget* parent);

    QString surfaceId() const { return m_surfaceId; }
    WorkspaceCanvas* canvas() const { return m_canvas; }

    // The surface label is the window title (mnemonics do not apply here —
    // titles are not menu text).
    void setSurfaceLabel(const QString& label);

    // Restore from the document's stored hint; on garbage or first open,
    // fall back to a sensible size centred on the anchor's screen.  A
    // restored position naming a disconnected monitor is clamped back onto
    // a live one — the FloatingContainerWindow::restoreAndEnsureVisible
    // discipline, the only such guard in the tree.
    void restoreFromHint(const QByteArray& hint, QWidget* anchor);

    // The live hint, for persisting (QWidget::saveGeometry form).
    QByteArray currentGeometryHint() const;

    // Orderly teardown: stop the debounce, emit one final hint, mark
    // shutting-down so the close is accepted instead of forwarded.
    void prepareShutdown();

signals:
    // The operator asked to close the window (title-bar close, Alt+F4).
    // Nothing has happened yet — the controller owns what closing means.
    void closeRequested(const QString& surfaceId);

    // Debounced move/resize stream (400 ms, the FloatingContainerWindow
    // cadence): the current saveGeometry() blob, for the document.
    void geometryHintChanged(const QString& surfaceId, const QByteArray& hint);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;

private:
    QString m_surfaceId;
    WorkspaceCanvas* m_canvas{nullptr};
    // Present only in frameless mode: the window's move/close chrome.
    // With native decorations the desktop provides both and a second
    // bar would be duplication.
    FramelessWindowTitleBar* m_titleBar{nullptr};
    QVBoxLayout* m_layout{nullptr};
    QTimer m_hintTimer;
    bool m_restoring{false};
    bool m_shuttingDown{false};
};

}  // namespace AetherSDR
