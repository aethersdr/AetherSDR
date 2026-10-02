#pragma once

// One additional canvas window (RFC #4887): a thin top-level hosting one
// WorkspaceCanvas. Follows FloatingContainerWindow's frameless/shutdown contract
// (flags re-applied before show, no WA_DeleteOnClose, screen-clamped restore,
// debounced geometry save, explicit prepareShutdown(); #2495/#4803). Its geometry
// is only a hint (WorkspaceDocument.h). Close is hide-and-keep: closeEvent is
// forwarded to the controller, never acted on; only prepareShutdown() closes.

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
