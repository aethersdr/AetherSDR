#pragma once

#include <QObject>
#include <QPointer>

class QDialog;
class QEvent;
class QWidget;

namespace AetherSDR {

class ThemeInspectorOverlay;

// Browser-devtools-style "click to find the token painting this region".
//
// Lifecycle is driven by ThemeEditorDialog.  When the operator clicks the
// "Inspect" toggle:
//   1. start() installs a QApplication-level event filter and shows a
//      transparent always-on-top overlay sized to the widget under the
//      cursor.
//   2. As the cursor moves, the overlay tracks the deepest widget at the
//      global position (skipping the editor dialog itself + the overlay).
//   3. Left-click captures (widget, local position), eats the event so it
//      never reaches the underlying widget, deactivates, and emits
//      widgetPicked() so the dialog can run tokensForWidget() and surface
//      the matches.
//   4. ESC cancels without picking.
//
// The overlay uses Qt::WindowTransparentForInput + WA_TransparentForMouseEvents
// so it never intercepts events itself — the global event filter is the
// authoritative event source.
class ThemeInspector : public QObject {
    Q_OBJECT
public:
    explicit ThemeInspector(QDialog* editorDialog, QObject* parent = nullptr);
    ~ThemeInspector() override;

    bool isActive() const { return m_active; }

public slots:
    void start();
    void stop();

signals:
    // Emitted on left-click during inspect mode.  `target` is the deepest
    // QWidget at the click point that wasn't part of the editor dialog;
    // `localPos` is the click position in `target`'s local coordinates.
    void widgetPicked(QWidget* target, QPoint localPos);

    // Emitted when the operator cancels inspect mode without picking
    // (ESC, or explicit stop() from the dialog).
    void canceled();

    // State-mirror signal so the dialog's toggle button stays in sync if
    // inspect mode self-exits (after a pick, after ESC, on hidden-dialog).
    void activeChanged(bool active);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QWidget* resolveTarget(const QPoint& globalPos) const;
    bool     belongsToEditor(QWidget* w) const;
    void     updateOverlay(QWidget* target);

    QDialog* m_editor;
    bool     m_active{false};
    QPointer<QWidget>      m_lastTarget;
    ThemeInspectorOverlay* m_overlay{nullptr};
};

} // namespace AetherSDR
