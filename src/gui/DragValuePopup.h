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
    explicit DragValuePopup(QWidget* parent = nullptr);

    void showValue(const QPoint& globalAnchor, const QString& text);
    void linger(int msec = 450);
    void hideNow();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPoint positionForAnchor(const QPoint& globalAnchor) const;

    QLabel* m_label{nullptr};
    QTimer* m_hideTimer{nullptr};
};

} // namespace AetherSDR
