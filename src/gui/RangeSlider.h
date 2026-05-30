#pragma once

#include <QWidget>

// Compact double-handle range slider.
//
// Renders a groove with a highlighted bar between two draggable handles.
// The current low/high values are drawn as labels on either side.
// Emits rangeChanged(int low, int high) on every change.
class RangeSlider : public QWidget {
    Q_OBJECT
public:
    explicit RangeSlider(int min, int max, int low, int high,
                         const QString& unit = {},
                         QWidget* parent = nullptr);

    int  low()  const { return m_low; }
    int  high() const { return m_high; }

    void setLow(int v);
    void setHigh(int v);
    void setRange(int min, int max);

    QSize sizeHint() const override { return {160, 20}; }
    QSize minimumSizeHint() const override { return {100, 18}; }

signals:
    void rangeChanged(int low, int high);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    int m_min, m_max, m_low, m_high;
    QString m_unit;

    static constexpr int kLabelW   = 30;   // px reserved each side for value text
    static constexpr int kGrooveH  =  4;   // groove height
    static constexpr int kHandleW  =  8;   // handle width
    static constexpr int kHandleH  = 14;   // handle height

    enum class Handle { None, Low, High } m_dragging{Handle::None};

    QRect grooveRect()        const;
    QRect handleRect(int val) const;
    int   valueToX(int val)   const;
    int   xToValue(int x)     const;
};
