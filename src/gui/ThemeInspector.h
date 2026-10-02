#pragma once

#include <QObject>
#include <QPointer>

class QDialog;
class QEvent;
class QWidget;

namespace AetherSDR {

class ThemeInspectorOverlay;

// "Click to find the token painting this region", driven by ThemeEditorDialog.
// start() installs an app-level event filter and an overlay that tracks the
// deepest widget under the cursor (skipping the editor and overlay). Left-click
// is eaten, deactivates and emits widgetPicked(); ESC cancels. The overlay is
// transparent for input; the global filter is the only event source.
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
