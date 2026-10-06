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
// part of it (through FramelessMoveHelper, like the frameless title bar), and
// Escape or the platform Close shortcut (⌘W, Ctrl+W) closes it. The title is
// still set for the taskbar and screen readers.
//
// Always frameless: it does not follow the View → Frameless Window setting,
// and it opens centred on its parent rather than restoring a saved geometry
// (see docs/style/dialog-patterns.md).
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
    static constexpr int kInset = 1;   // hairline border; the body sits inside it

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
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
// visible, and skips frames while its window is not exposed (minimised or
// covered). When the OS asks for reduced motion the spark holds still.
class SparkRing : public QWidget {
    Q_OBJECT

public:
    SparkRing(const QPixmap& image, int diameter, QWidget* parent = nullptr);
    QSize sizeHint() const override;

    // Follows the OS reduced-motion preference (QAccessibilityHints); public
    // so a test can apply a preference the platform does not report.
    void setMotionPreference(Qt::MotionPreference preference);
    bool isAnimating() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QPixmap m_image;
    int     m_diameter;
    qreal   m_angle{90.0};
    bool    m_motionReduced{false};
    QTimer* m_timer{nullptr};
};

// Wraps one widget (a button) and draws the guide's gold spark around it: a
// point of light in color.canon.sparkGold with a color.canon.sparkGoldHot tip
// circling a rounded border once every three seconds, like the Contributor
// Logbook's award cards. Gold marks recognition. Animates only while visible,
// skips frames while its window is not exposed, and holds still when the OS
// asks for reduced motion.
class SparkBorder : public QWidget {
    Q_OBJECT

public:
    // radius: the wrapped widget's corner radius; the spark runs just outside it.
    SparkBorder(QWidget* child, int radius, QWidget* parent = nullptr);

    // Follows the OS reduced-motion preference (QAccessibilityHints); public
    // so a test can apply a preference the platform does not report.
    void setMotionPreference(Qt::MotionPreference preference);
    bool isAnimating() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QWidget* m_child;
    int      m_radius;
    qreal    m_angle{90.0};
    bool     m_motionReduced{false};
    QTimer*  m_timer{nullptr};
};

} // namespace AetherSDR
