#pragma once

#include <QString>
#include <QWidget>

class QVBoxLayout;

namespace AetherSDR {

class ContainerTitleBar;

// Generic dockable / floatable wrapper widget — the unit the container
// system operates on.  Exactly one QWidget lives in its body via
// setContent(); the widget's float/dock/visibility state is managed
// independently of its content.
//
// Phase 1 scope: single-level (no child containers), just the basic
// header + body + state machine + signals.  Nesting and ContainerManager
// integration are added in later phases.
class ContainerWidget : public QWidget {
    Q_OBJECT

public:
    enum class DockMode {
        PanelDocked,   // inside its parent's body layout
        Floating,      // owned by a FloatingContainerWindow
    };

    explicit ContainerWidget(const QString& id,
                             const QString& title,
                             QWidget* parent = nullptr);
    ~ContainerWidget() override;

    // Stable identifier used by the manager for path-based lookup
    // and by persistence to key each container's state.
    QString id() const { return m_id; }

    // Human-readable title shown in the header bar.  Changing it
    // updates the titlebar in place.
    void setTitle(const QString& title);
    QString title() const;

    // Install the single primary content widget.  Convenience for
    // leaf containers that only ever hold one thing.  Internally
    // this is just addChildWidget() of that widget; the pointer is
    // cached so content() remains accessible after reparents.  A
    // second call replaces the first (previous is returned).
    QWidget* setContent(QWidget* content);
    QWidget* content() const { return m_content; }

    // ── Child-widget API (used by nesting) ───────────────────────
    //
    // A container's body is a QVBoxLayout; children stack vertically
    // in insertion order.  Children can be leaf widgets or other
    // ContainerWidgets — the container itself doesn't distinguish
    // (ContainerManager tracks parent/child relationships for
    // docking logic).
    //
    // insertChildWidget(-1, w)  → append
    // insertChildWidget(i, w)   → insert at index i (clamped)
    void insertChildWidget(int index, QWidget* child);
    void removeChildWidget(QWidget* child);
    int  childWidgetCount() const;
    QWidget* childWidgetAt(int index) const;
    int  indexOfChildWidget(QWidget* child) const;

    // Dock state.  UI code reads these to render the correct button
    // labels; programmatic transitions use requestFloat / requestDock.
    DockMode dockMode() const { return m_dockMode; }
    bool isFloating() const     { return m_dockMode == DockMode::Floating; }
    bool isPanelDocked() const  { return m_dockMode == DockMode::PanelDocked; }

    // Logical visibility — distinct from QWidget::isVisible, which
    // reflects the current layout state (a panel-docked container is
    // "invisible" whenever its parent chooses not to show it).  This
    // is the user's "show this container" flag, persisted separately.
    void setContainerVisible(bool visible);
    bool isContainerVisible() const { return m_visible; }

    // Access to the titlebar — callers can hide its close button
    // for root containers or customise the title dynamically.
    ContainerTitleBar* titleBar() { return m_titleBar; }

    // Hide / show the container's own titlebar.  Useful when a
    // container is nested inside a legacy AppletPanel wrapper that
    // already provides an outer titlebar — avoids stacking two.
    void setTitleBarVisible(bool visible);

    // Returns the widget that should be inserted into a parent layout
    // when this container is panel-docked.  Currently that's just
    // `this` — kept as a separate API so future phases can introduce
    // intermediate wrappers without breaking callers.
    QWidget* dockWidget() { return this; }

signals:
    // Emitted by the titlebar's float/dock button.  The connected
    // slot (usually ContainerManager) does the actual reparenting
    // work; the widget itself doesn't know about FloatingContainerWindow.
    void floatRequested();
    void dockRequested();

    // Emitted when the user clicks the close (×) button in the
    // titlebar.  Connected code typically calls setContainerVisible(false)
    // but may choose to destroy the container instead.
    void closeRequested();

    // Emitted when the titlebar's pin button toggles.  Only fires
    // while the container is in floating mode (the pin is hidden
    // otherwise).  Manager applies the hint and persists the choice.
    void alwaysOnTopToggled(bool on);

    // Fired after setContainerVisible() changes state.
    void visibilityChanged(bool visible);

    // Fired after dockMode() changes — manager uses this to keep the
    // titlebar button label in sync.
    void dockModeChanged(DockMode mode);

private slots:
    void onTitleBarFloatToggle();
    void onTitleBarClose();
    void onTitleBarDragStart(const QPoint& globalPos);

private:
    // Internal — ContainerManager flips this when the container
    // finishes a dock transition.  Emits dockModeChanged.
    friend class ContainerManager;
    friend class FloatingContainerWindow;
    void setDockMode(DockMode mode);

    QString            m_id;
    ContainerTitleBar* m_titleBar{nullptr};
    QWidget*           m_body{nullptr};
    QVBoxLayout*       m_bodyLayout{nullptr};
    QWidget*           m_content{nullptr};
    DockMode           m_dockMode{DockMode::PanelDocked};
    bool               m_visible{true};
};

} // namespace AetherSDR
