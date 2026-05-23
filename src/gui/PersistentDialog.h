#pragma once

#include <QDialog>

class QCloseEvent;
class QMoveEvent;
class QResizeEvent;
class QShowEvent;

namespace MasterSDR {

class FramelessWindowTitleBar;

// Base class that collapses the lazy-construct + non-modal + geometry-persist
// + frameless-chrome boilerplate shared by every persistent dialog in
// MainWindow (Profile Manager, Memory, SpotHub, DSP, etc.).  Subclasses fill
// in their content via bodyWidget() and the base class owns:
//
//   * The outer (titleBar + bodyWidget) layout and frameless title-bar widget.
//   * FramelessResizer installation (8-axis resize on the top-level window).
//   * setFramelessMode(bool) — flips Qt::FramelessWindowHint while preserving
//     geometry and toggles the custom title bar / body margins.
//   * Geometry persistence under an AppSettings key.  Save fires on close
//     (canonical, disk-flushed) AND on move/resize (in-memory, crash-resilient
//     — matches ProfileManagerDialog's existing contract).  Restore is
//     deferred to first showEvent() so subclasses can call setMinimumSize()
//     in their own constructor without the just-restored geometry being
//     clipped.
//
// Subclass usage:
//   class FooDialog : public PersistentDialog {
//   public:
//       explicit FooDialog(QWidget* parent = nullptr)
//         : PersistentDialog("Foo", "FooDialogGeometry", parent) {
//           auto* root = new QVBoxLayout(bodyWidget());
//           // ... dialog content ...
//       }
//   };
class PersistentDialog : public QDialog {
    Q_OBJECT

public:
    // title:   Window title (and frameless chrome label).
    // geomKey: AppSettings key for geometry persistence.  Empty → no persist.
    explicit PersistentDialog(const QString& title,
                              const QString& geomKey,
                              QWidget* parent = nullptr);

    void setFramelessMode(bool on);

    // Content goes here.  Subclasses install their own QLayout on this widget.
    QWidget* bodyWidget() const { return m_body; }

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
    FramelessWindowTitleBar* m_titleBar{nullptr};
    QWidget*                 m_body{nullptr};
    bool                     m_framelessOn{false};
    bool                     m_restoringGeometry{false};
    bool                     m_geometryRestored{false};
};

} // namespace MasterSDR
