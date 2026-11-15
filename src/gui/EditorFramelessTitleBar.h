#pragma once

#include <QWidget>

class QLabel;

namespace AetherSDR {

// 20 px title bar for the frameless PooDoo Audio editor windows. Drag moves
// the window (macOS uses a manual move: repeated startSystemMove() can be
// refused for seconds after a move). Double-click toggles maximize. The — □ ✕
// labels map to showMinimized / showMaximized / close via event filters.
// setTitleText() lets the host relabel on Side/Path change.
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
