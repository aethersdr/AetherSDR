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
// and it asks to open centred on its parent rather than restoring a saved
// geometry (see docs/style/dialog-patterns.md). Wayland compositors place
// top-level windows themselves, so there the request may be ignored.
//
// Corners are transparent, so the rounding needs a compositing window manager
// (always on macOS, Windows and Wayland; an X11 session without a compositor
// shows square black corners).
class CanonWindow : public QDialog {
    Q_OBJECT

public:
    // Dialog: a short-lived window on top of its parent (About, Waveforms).
    // Workspace: a tool the operator works in for a session (AetherRX,
    // AetherTX): an independent top-level window, with its own taskbar entry,
    // that minimises separately and can live on another monitor. In a
    // workspace Return and Enter never click a default button (see
    // setReturnClicksDefault()).
    enum class Kind { Dialog, Workspace };

    explicit CanonWindow(const QString& title, QWidget* parent = nullptr,
                         Kind kind = Kind::Dialog);

    // Content goes here; install a layout on it.
    QWidget* bodyWidget() const { return m_body; }

    // Opt in to saving size and position under an AppSettings key, for a
    // canon window that is a workspace tool rather than a short-lived one
    // (Network Diagnostics). The saved geometry replaces the centred
    // placement; with no key, or nothing saved yet, the window centres.
    void setGeometryKey(const QString& key) { m_geometryKey = key; }

    // Open at this size every time, whatever size was saved (the position is
    // still restored and saved). Applied before the window centres, so a
    // window that cannot restore its position centres at this size.
    void setLaunchSize(const QSize& size);

    // Whether Return and Enter in a field click the default button, as a
    // QDialog's do. Off by default in a workspace, on in a dialog. A page of
    // controls is not a form: QDialog makes every push button an
    // auto-default, so Return in a field would also press the first button
    // (a stage tab, a mode card). A focused button still takes Return itself.
    void setReturnClicksDefault(bool on) { m_returnClicksDefault = on; }

    // The owner positions the window before showing it (the Connect window,
    // anchored above the status bar), so the first show does not centre it.
    void setOwnerPlaced(bool on) { m_ownerPlaced = on; }

    static constexpr int kRadius = 16;
    static constexpr int kInset = 1;   // hairline border; the body sits inside it

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void paintGround(QPaintDevice& device) const;
    void saveGeometryToSettings();
    enum class Restored { Nothing, SizeOnly, SizeAndPosition };
    Restored restoreGeometryFromSettings();

    bool         m_returnClicksDefault;
    bool         m_ownerPlaced{false};
    QWidget*     m_body{nullptr};
    QToolButton* m_close{nullptr};
    bool         m_placed{false};
    QString      m_geometryKey;
    QSize        m_launchSize;
    bool         m_restoringGeometry{false};
    QPixmap      m_ground;   // the painted ground, rebuilt on resize, DPR or theme change
};

// The canon header a CanonWindow carries in place of a title bar: the title in
// canon ink over a hairline, with a right margin that keeps it clear of the
// corner close button. The empty part of it is a handle for moving the window.
QWidget* makeCanonHeader(const QString& title);

// The guide's spark: a point of light circling a path, with a bright tip and a
// tail that fades to nothing, over a wider faint glow. The base owns the
// animation: it runs only while the widget is visible, ticks slowly while its
// window is not exposed (minimised or covered), and holds still when the OS
// asks for reduced motion (QAccessibilityHints::motionPreference).
class SparkWidget : public QWidget {
    Q_OBJECT

public:
    // Follows the OS preference; public so a test can apply one the platform
    // does not report.
    void setMotionPreference(Qt::MotionPreference preference);
    bool isAnimating() const;

protected:
    SparkWidget(int lapMs, QWidget* parent);

    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    // Strokes `path` with the spark centred on `centre` at the current angle:
    // a `glowWidth` pass at `glowAlpha`, then a `coreWidth` pass at full
    // strength.
    void paintSpark(QPainter& p, const QPainterPath& path, const QPointF& centre,
                    const QColor& body, const QColor& tip,
                    qreal glowWidth, qreal glowAlpha, qreal coreWidth) const;

private:
    void tick();

    int     m_lapMs;
    qreal   m_angle{90.0};
    bool    m_motionReduced{false};
    QTimer* m_timer{nullptr};
};

// A circular image (a logo, an avatar) with a contrast ring and a cyan spark
// (color.canon.cyan with a color.canon.sparkHot tip) circling it once every
// seven seconds. The image is scaled at paint time for the screen's device
// pixel ratio, so it stays sharp when the window moves between screens.
class SparkRing : public SparkWidget {
    Q_OBJECT

public:
    SparkRing(const QPixmap& image, int diameter, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPixmap m_source;
    QPixmap m_scaled;
    qreal   m_scaledDpr{0.0};
    int     m_diameter;
};

// Wraps one widget (a button) and draws a gold spark (color.canon.sparkGold
// with a color.canon.sparkGoldHot tip) around it, circling a rounded border
// once every three seconds, like the Contributor Logbook's award cards. Gold
// marks recognition.
class SparkBorder : public SparkWidget {
    Q_OBJECT

public:
    // radius: the wrapped widget's corner radius; the spark runs just outside it.
    SparkBorder(QWidget* child, int radius, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QWidget* m_child;
    int      m_radius;
};

} // namespace AetherSDR
