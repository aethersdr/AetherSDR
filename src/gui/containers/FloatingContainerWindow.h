#pragma once

#include <QString>
#include <QTimer>
#include <QWidget>

class QVBoxLayout;

namespace AetherSDR {

class ContainerWidget;

// Top-level window that hosts a ContainerWidget in floating mode.
// One FloatingContainerWindow per floating container.  The window's
// layout is a single-slot QVBoxLayout holding the container; the
// container's own titlebar serves as the drag handle and close/dock
// affordance so the window's native title bar can be bare.
//
// Geometry is saved to AppSettings under `geometryKey()` whenever the
// window moves or resizes; ContainerManager assigns that key from the
// container's ID once Phase 2 lands.  For Phase 1 tests the caller
// passes the key directly.
class FloatingContainerWindow : public QWidget {
    Q_OBJECT

public:
    explicit FloatingContainerWindow(QWidget* parent = nullptr);
    ~FloatingContainerWindow() override;

    // Take ownership of a ContainerWidget — it becomes the sole
    // content of this window and its dockMode flips to Floating.
    // Pass null to clear.
    void takeContainer(ContainerWidget* container);

    // Release the hosted container without destroying it — caller
    // re-parents it back into a panel layout.  After release this
    // window typically destroys itself via deleteLater().
    ContainerWidget* releaseContainer();
    ContainerWidget* container() const { return m_container; }

    // AppSettings key under which to store geometry (serialized as
    // base64 of QByteArray returned by saveGeometry()).  Empty = no
    // persistence.
    void setGeometryKey(const QString& key);
    QString geometryKey() const { return m_geometryKey; }

    // Called by ContainerManager before the app exits.  Saves geometry,
    // suppresses the dock-on-close behaviour, and closes the window.
    void prepareShutdown();

    // Apply or remove Qt::FramelessWindowHint at runtime to match the
    // main-window frameless setting.  Preserves geometry and visibility.
    void setFramelessMode(bool on);

    // Toggle Qt::WindowStaysOnTopHint at runtime — see issue #2430.
    // Same recreate-the-native-window dance as setFramelessMode():
    // snapshot geometry + visibility, flip the bit, restore.
    void setAlwaysOnTop(bool on);
    bool isAlwaysOnTop() const { return m_alwaysOnTop; }

    // Restore the window's geometry from the key, clamping to a
    // visible screen.  If no saved geometry exists, centres on the
    // anchor's current screen at a reasonable default size.  Safe to
    // call before show().
    void restoreAndEnsureVisible(QWidget* anchor);

signals:
    // Emitted when the user asks to dock (titlebar dock button or
    // window close event).  Connected code pulls the container out
    // via releaseContainer() and reparents it back to the panel.
    void dockRequested(ContainerWidget* container);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;

private:
    void saveGeometryToKey() const;

    ContainerWidget* m_container{nullptr};
    QVBoxLayout*     m_layout{nullptr};
    QString          m_geometryKey;
    bool             m_restoring{false};
    bool             m_shuttingDown{false};
    bool             m_alwaysOnTop{false};
    QTimer           m_saveTimer;
};

} // namespace AetherSDR
