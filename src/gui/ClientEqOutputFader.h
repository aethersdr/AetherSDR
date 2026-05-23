#pragma once

#include <QWidget>

class QLineEdit;

namespace MasterSDR {

// Combined vertical fader + level meter.  One custom-painted bar shows
// the post-EQ peak level as a gradient fill rising from the bottom, with
// a horizontal handle marker at the current output-gain position.  Drag
// the handle up/down to change gain, double-click to reset to 0 dB,
// scroll wheel for fine 0.5 dB steps.
//
// The widget writes a linear gain [0.0, ~4.0] back via gainChanged(); the
// editor's existing FFT timer feeds in a peak at ~25 Hz so the meter
// stays lively without a separate polling loop.
class ClientEqOutputFader : public QWidget {
    Q_OBJECT

public:
    explicit ClientEqOutputFader(QWidget* parent = nullptr);

    void setGainLinear(float linear);
    float gainLinear() const { return m_gain; }

    void setPeakLinear(float peakLinear);

signals:
    void gainChanged(float linear);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void refreshValueLabel();
    void commitValueEdit();
    void setGainFromY(int y);

    QLineEdit* m_valueEdit{nullptr};
    float   m_gain{1.0f};
    float   m_smoothedPeak{-120.0f};  // dB
    bool    m_dragging{false};

    // dB ranges
    static constexpr float kGainMinDb  = -36.0f;
    static constexpr float kGainMaxDb  = +12.0f;
    static constexpr float kMeterMinDb = -60.0f;
    static constexpr float kMeterMaxDb =   0.0f;

    // Layout constants
    static constexpr int kLabelColW = 20;
    static constexpr int kGap       = 2;
    static constexpr int kBarW      = 16;
    static constexpr int kHandleOverhang = 4;   // handle sticks out on each side
    static constexpr int kHandleH        = 3;

    // Vertical padding inside the fader strip so the handle can reach the
    // top / bottom ends without clipping.
    static constexpr int kStripTopPad    = 4;
    static constexpr int kStripBottomPad = 4;

    // Cached strip rect — recomputed in paintEvent.  Used by mouse handlers
    // so they don't recompute geometry on every move.
    int m_stripTop{0};
    int m_stripH{0};
};

} // namespace MasterSDR
