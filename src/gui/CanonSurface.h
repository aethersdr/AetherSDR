#pragma once

#include <QPixmap>
#include <QWidget>

class QBoxLayout;
class QToolButton;

namespace AetherSDR {

// The canon for a surface that lives inside the main window rather than in a
// window of its own: the same ground, hairline border and round close button
// as CanonWindow (see paintCanonGround()), with square corners, because the
// main window's frame already holds it. Use one of the three kinds below
// rather than this base, so each can grow its own chrome later.
//
// The window-only behavior stays with CanonWindow: a surface does not move
// when dragged, and Escape and ⌘W / Ctrl+W do not close it. Its close button
// only emits closeRequested(); the owner decides what closing means (hide the
// applet, remove the panadapter) and whether the button shows at all, as the
// container and panadapter title bars do today.
class CanonSurface : public QWidget {
    Q_OBJECT

public:
    // Content goes here; install a layout on it.
    QWidget* bodyWidget() const { return m_body; }

    // The close button shows by default. An owner hides it where closing is
    // not allowed (the only panadapter, a canvas inside an applet).
    void setCloseButtonVisible(bool visible);
    bool isCloseButtonVisible() const;
    QToolButton* closeButton() const { return m_close; }

    // Moves the close button out of the corner into the owner's own header
    // row (a panadapter's title strip), at the end of `row`. Visibility and
    // closeRequested() are unchanged.
    void placeCloseButtonIn(QBoxLayout* row);

    static constexpr int kInset = 1;   // hairline border; the body sits inside it

signals:
    void closeRequested();

protected:
    explicit CanonSurface(QWidget* parent);

    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget*     m_body{nullptr};
    QToolButton* m_close{nullptr};
    bool         m_closeInCorner{true};
    QPixmap      m_ground;   // the painted ground, rebuilt on resize, DPR or theme change
};

// An applet in the applet panel.
class CanonApplet : public CanonSurface {
    Q_OBJECT

public:
    explicit CanonApplet(QWidget* parent = nullptr) : CanonSurface(parent) {}
};

// A panadapter: the spectrum and waterfall for one pan.
class CanonPan : public CanonSurface {
    Q_OBJECT

public:
    explicit CanonPan(QWidget* parent = nullptr) : CanonSurface(parent) {}
};

// A drawing surface inside the main window (an editor curve, a meter face).
class CanonCanvas : public CanonSurface {
    Q_OBJECT

public:
    explicit CanonCanvas(QWidget* parent = nullptr) : CanonSurface(parent) {}
};

} // namespace AetherSDR
