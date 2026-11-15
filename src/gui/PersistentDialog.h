#pragma once

#include <QDialog>
#include <QMargins>

class QCloseEvent;
class QMoveEvent;
class QResizeEvent;
class QShowEvent;

namespace AetherSDR {

class FramelessWindowTitleBar;

// Base for persistent non-modal dialogs: owns the titleBar + bodyWidget()
// layout, FramelessResizer, setFramelessMode(bool) (keeps geometry), and
// geometry persistence under an AppSettings key — saved on close (flushed) and
// on move/resize (in memory), restored on first showEvent() so a subclass's
// setMinimumSize() cannot clip it. Subclass: pass (title, geometryKey, parent)
// and build content on bodyWidget().
class PersistentDialog : public QDialog {
    Q_OBJECT

public:
    // title:   Window title (and frameless chrome label).
    // geomKey: AppSettings key for geometry persistence.  Empty → no persist.
    // toolWindow: keep the window out of the taskbar and floating above its
    //   parent (Qt::Tool) instead of the default dialog window type (Qt::Dialog).
    //   Preserved across the runtime frameless toggle.
    explicit PersistentDialog(const QString& title,
                              const QString& geomKey,
                              QWidget* parent = nullptr,
                              bool toolWindow = false);

    void setFramelessMode(bool on);

    // Content goes here.  Subclasses install their own QLayout on this widget.
    QWidget* bodyWidget() const { return m_body; }
    // Override the standard 9 px body inset for dialogs whose content owns a
    // deliberate edge-to-edge or wider layout.  Call after installing the
    // body layout; the values are also reapplied after runtime chrome toggles.
    void setBodyLayoutMargins(const QMargins& framed, const QMargins& frameless);

protected:
    void closeEvent(QCloseEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    void applyBodyLayoutMargins();

    QString                  m_geomKey;
    // Base window type ORed back in after every setWindowFlags() — Qt::Dialog by
    // default, Qt::Tool for tool windows. setFramelessMode() strips the window
    // type bits, so this is how the choice survives a runtime chrome toggle.
    Qt::WindowType           m_windowType{Qt::Dialog};
    FramelessWindowTitleBar* m_titleBar{nullptr};
    QWidget*                 m_body{nullptr};
    bool                     m_framelessOn{false};
    bool                     m_restoringGeometry{false};
    bool                     m_geometryRestored{false};
    QMargins                 m_framedBodyMargins{9, 9, 9, 9};
    QMargins                 m_framelessBodyMargins{9, 7, 9, 9};
};

} // namespace AetherSDR
