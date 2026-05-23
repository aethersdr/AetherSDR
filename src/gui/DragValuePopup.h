#pragma once

#include <QPoint>
#include <QString>
#include <QWidget>

class QLabel;
class QTimer;

namespace AetherSDR {

// Small floating readout shown while a value control is being dragged.
// This deliberately avoids QToolTip so the lifetime, position, and styling
// are consistent across macOS, Windows, and Linux window managers.
class DragValuePopup : public QWidget {
public:
    // Brief "memory cue" duration after the user releases the slider.  Long
    // enough to register visually, short enough not to obscure subsequent
    // interaction.  PR #2944 review settled on 450 ms after testing 250 ms
    // (too fleeting), 450 ms (right), and 750 ms (felt sluggish).
    static constexpr int kDefaultLingerMs = 450;

    explicit DragValuePopup(QWidget* parent = nullptr);

    void showValue(const QPoint& globalAnchor, const QString& text);
    void linger(int msec = kDefaultLingerMs);
    void hideNow();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPoint positionForAnchor(const QPoint& globalAnchor) const;

    QLabel* m_label{nullptr};
    QTimer* m_hideTimer{nullptr};
};

} // namespace AetherSDR
