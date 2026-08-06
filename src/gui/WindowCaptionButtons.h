#pragma once

#include <QAbstractButton>
#include <QVariantMap>
#include <QWidget>

class QHBoxLayout;

namespace AetherSDR {

// Which window-control language to draw.  Every platform draws its own — the
// window is frameless on all three, so there is never a native control to defer
// to.  The three variants differ only in shape and placement; the behaviour and
// the accessibility contract are identical.
enum class CaptionStyle {
    WindowsCaption,   // design 1e — full-bar-height 46 px glyph buttons, right-aligned
    LinuxChips,       // design 1b — 13 px bordered chips that fill on hover
    MacTrafficLights  // 12 px round lights at the left, glyphs on group hover
};

// One caption control.  QAbstractButton so it is focusable and screen-reader
// named; the glyph is painted rather than set as text because the spec calls
// for 1 px stroked paths at a fixed 11 px, which no font guarantees.
class CaptionButton : public QAbstractButton {
    Q_OBJECT

public:
    enum class Role { Minimize, MaximizeRestore, Close };

    CaptionButton(Role role, CaptionStyle style, QWidget* parent = nullptr);

    Role role() const { return m_role; }

    // Restore glyph (two offset squares) instead of the maximize square.
    void setMaximized(bool maximized);

    // Windows answers WM_NCHITTEST with HTMAXBUTTON over the maximize control
    // so Snap Layouts opens on hover — but that also means the OS routes the
    // mouse to the non-client area and Qt never sees enter/leave here.  The
    // native handler drives the hover state through this instead.
    void setForcedHover(bool hovered);

    // Traffic lights only: on macOS hovering ANY light reveals the glyphs on
    // ALL three, and an inactive window greys the whole cluster.  Both are
    // cluster-wide states, so the cluster pushes them down.
    void setGroupHovered(bool hovered);
    void setWindowActive(bool active);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void enterEvent(QEnterEvent* ev) override;
    void leaveEvent(QEvent* ev) override;
    void focusInEvent(QFocusEvent* ev) override;
    void focusOutEvent(QFocusEvent* ev) override;

private:
    bool  isHot() const { return m_hovered || m_forcedHover; }
    // "Focus-visible": the ring is drawn only when focus arrived from the
    // keyboard.  Qt hands the initial focus to the first widget in the tab
    // order, which is this cluster, so painting on bare hasFocus() opened every
    // window with a ring around a traffic light.
    bool  m_focusVisible{false};
    void  paintGlyph(QPainter& p, const QColor& color) const;
    void  paintTrafficLight(QPainter& p) const;

    Role         m_role;
    CaptionStyle m_style;
    bool         m_maximized{false};
    bool         m_hovered{false};
    bool         m_forcedHover{false};
    bool         m_groupHovered{false};
    bool         m_windowActive{true};
};

// The caption-control cluster for the platforms that draw their own: Windows
// (design 1e) and Linux (design 1b).
class WindowCaptionButtons : public QWidget {
    Q_OBJECT

public:
    explicit WindowCaptionButtons(CaptionStyle style, QWidget* parent = nullptr);

    void setMaximized(bool maximized);

    // Global-coordinate rect of the maximize control, for the Windows
    // WM_NCHITTEST HTMAXBUTTON answer.  Null rect when there is no such
    // control (never, today — kept total so callers need no style check).
    QRect maximizeButtonGlobalRect() const;
    void  setMaximizeForcedHover(bool hovered);

    CaptionStyle style() const { return m_style; }

    // Called by the buttons themselves as the pointer moves through the
    // cluster, so the macOS lights can reveal their glyphs as a group.
    void childHoverChanged(bool entered);

    // Introspection for the automation bridge (`titlebar` model).
    QVariantMap state() const;

protected:
    void showEvent(QShowEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

signals:
    void minimizeRequested();
    void maximizeRestoreRequested();
    void closeRequested();

private:
    CaptionStyle   m_style;
    QHBoxLayout*   m_layout{nullptr};
    CaptionButton* m_minimize{nullptr};
    CaptionButton* m_maximize{nullptr};
    CaptionButton* m_close{nullptr};
    int            m_hoveredChildren{0};
};

} // namespace AetherSDR
