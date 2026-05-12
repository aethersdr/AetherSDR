#pragma once

#include <QWidget>

class QLabel;

namespace AetherSDR {

// Reusable 20 px-tall title bar for the PooDoo Audio editor windows
// (parametric EQ, compressor, gate, tube, PUDU, reverb, de-esser).
// Each editor sets Qt::FramelessWindowHint at construction and adds
// this widget at the very top of its layout.  Behaviour:
//
//  - Press-and-drag anywhere on the bar moves the window.  macOS uses
//    a manual move path because repeated QWindow::startSystemMove()
//    calls can be refused for several seconds after a move completes.
//  - Double-click toggles maximize.
//  - The trio at the right (— □ ✕) wires to showMinimized /
//    showMaximized / close on the host window via an installed event
//    filter on each glyph QLabel.
//  - setTitleText() drives the heading on the left so the host editor
//    can flip the label when its Side / Path changes.
class EditorFramelessTitleBar : public QWidget {
public:
    explicit EditorFramelessTitleBar(QWidget* parent = nullptr);

    void setTitleText(const QString& text);

    // Hide the min / max / close trio while leaving the title label
    // visible.  Used by the AetherialAudioStrip when it embeds the
    // per-stage editors — the strip owns its own window controls, so
    // each embedded panel just needs the name plate (#2301).
    void setControlsVisible(bool on);

protected:
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QLabel* m_titleLbl{nullptr};
    QLabel* m_minLbl{nullptr};
    QLabel* m_maxLbl{nullptr};
    QLabel* m_closeLbl{nullptr};
};

} // namespace AetherSDR
