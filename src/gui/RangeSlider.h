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
    // label: short prefix drawn left of the low value (e.g. "Pitch", "WPM")
    // unit:  suffix appended to each value (e.g. " Hz")
    explicit RangeSlider(int min, int max, int low, int high,
                         const QString& label = {},
                         const QString& unit  = {},
                         QWidget* parent = nullptr);

    int  low()  const { return m_low; }
    int  high() const { return m_high; }

    void setLow(int v);
    void setHigh(int v);
    void setRange(int min, int max);

    QSize sizeHint()        const override { return {m_leftLabelW + 100 + kRightLabelW, 20}; }
    QSize minimumSizeHint() const override { return {m_leftLabelW +  60 + kRightLabelW, 18}; }

signals:
    void rangeChanged(int low, int high);

protected:
    bool event(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;

private:
    int m_min, m_max, m_low, m_high;
    QString m_label;
    QString m_unit;

    static constexpr int kRightLabelW = 30;  // px for right value text
    static constexpr int kGrooveH     =  4;
    static constexpr int kHandleW     =  8;
    static constexpr int kHandleH     = 14;
    int m_leftLabelW{30};                    // wider when m_label is set

    enum class Handle { None, Low, High };
    Handle m_dragging{Handle::None};
    Handle m_focused {Handle::None};  // handle that responds to keyboard

    QRect grooveRect()        const;
    QRect handleRect(int val) const;
    int   valueToX(int val)   const;
    int   xToValue(int x)     const;
};
