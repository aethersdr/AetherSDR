#pragma once

#include <QDialog>
#include <QPixmap>

class QTimer;
class QToolButton;

namespace AetherSDR {

// A window in the AetherSDR style guide, replacing the FramelessWindowTitleBar
// chrome for windows that adopt it: no title bar, 16 px rounded corners, the
// guide's ambient ground (color.canon.ground with a blue and a teal bloom and a
// fading 64 px grid) and a hairline color.canon.lineHi border. A round close
// button sits in the top-right corner; the window moves by dragging any empty
// part of it, and Escape closes it. The title is still set for the taskbar and
// screen readers.
//
// Corners are transparent, so the rounding needs a compositing window manager
// (always on macOS, Windows and Wayland; an X11 session without a compositor
// shows square black corners).
class CanonWindow : public QDialog {
    Q_OBJECT

public:
    explicit CanonWindow(const QString& title, QWidget* parent = nullptr);

    // Content goes here; install a layout on it.
    QWidget* bodyWidget() const { return m_body; }

    static constexpr int kRadius = 16;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QWidget*     m_body{nullptr};
    QToolButton* m_close{nullptr};
    bool         m_placed{false};
};

// A circular image (a logo, an avatar) with a contrast ring and the guide's
// spark: a point of light circling the ring once every seven seconds, in
// color.canon.cyan with a color.canon.sparkHot tip. Animates only while
// visible.
class SparkRing : public QWidget {
    Q_OBJECT

public:
    SparkRing(const QPixmap& image, int diameter, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QPixmap m_image;
    int     m_diameter;
    qreal   m_angle{90.0};
    QTimer* m_timer{nullptr};
};

} // namespace AetherSDR
