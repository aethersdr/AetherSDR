#pragma once

#include <QWidget>
#include <QPainter>
#include <QMouseEvent>

namespace AetherSDR {

// Visual filter passband display with drag-to-adjust interaction.
// Static trapezoid shape with numeric lo/hi/bandwidth labels.
// Horizontal drag shifts the passband, vertical drag adjusts width.
// Matches SmartSDR's RX panel filter widget behavior.
class FilterPassbandWidget : public QWidget {
    Q_OBJECT

public:
    explicit FilterPassbandWidget(QWidget* parent = nullptr);

    void setFilter(int lo, int hi);
    void setMode(const QString& mode);
    void setWidthRange(int minimumHz, int maximumHz, int stepHz);

    int filterLo() const { return m_lo; }
    int filterHi() const { return m_hi; }

    QSize minimumSizeHint() const override { return {120, 36}; }
    QSize sizeHint() const override { return {200, 60}; }

signals:
    void filterChanged(int lo, int hi);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;

private:
    int m_lo{100};
    int m_hi{2800};
    QString m_mode{"USB"};
    int m_minimumWidthHz{50};
    int m_maximumWidthHz{0};  // no advertised limit: preserve legacy gestures
    int m_widthStepHz{50};

    enum DragMode { DragNone, DragShift, DragLo, DragHi };
    DragMode m_dragMode{DragNone};
    QPoint m_dragStartPos;
    int m_dragStartLo{0};
    int m_dragStartHi{0};

};

} // namespace AetherSDR
