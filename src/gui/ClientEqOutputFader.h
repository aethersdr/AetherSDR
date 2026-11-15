#pragma once

#include "MeterExtremes.h"

#include <QElapsedTimer>
#include <QRectF>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPainter;

namespace AetherSDR {

// Combined fader + level meter, vertical or horizontal.  One custom-painted bar shows
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

    // Vertical runs bottom-to-top beside the EQ; horizontal runs
    // left-to-right beneath it, which is what lets the graph have the whole
    // width of the window. Changing it rebuilds the label layout, so set it
    // once after construction rather than per frame.
    void setOrientation(Qt::Orientation orientation);
    Qt::Orientation orientation() const { return m_orientation; }

    void setGainLinear(float linear);
    float gainLinear() const { return m_gain; }

    void setPeakLinear(float peakLinear);

    // Whether this strip sets the EQ's master gain as well as reporting the
    // level. Off leaves a meter: no handle, no drag, no editable value, and the
    // gain it would have written stays where it is. The receive chain uses that
    // -- its EQ runs at unity and its band gains are where its level is set --
    // while transmit keeps the control.
    void setGainControlEnabled(bool enabled);
    bool gainControlEnabled() const { return m_gainControl; }

signals:
    void gainChanged(float linear);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void refreshValueLabel();
    void refreshLevelLabel();
    void commitValueEdit();
    // Position along the strip, in whichever axis the orientation runs.
    void setGainFromPos(QPoint pos);
    void rebuildLabelLayout();
    void paintVertical(QPainter& p);
    void paintHorizontal(QPainter& p);
    // A level in dB to its hole-local position in SmartMTR UNITS.
    double posUnitsForDb(double db) const;
    // Where the hole sits, in pixels, for the current size and font. Paint and
    // the label layout both read it so the caps cannot drift off the meter.
    QRectF holeRect() const;
    // Keep the "OUT" and value caps centred on the hole rather than on the
    // widget, which the ticks and labels above push them off.
    void centreCapsOnHole();

    QLineEdit* m_valueEdit{nullptr};
    // Live level readout, shown where the gain field would be on a strip that
    // has no gain to set. Never both: one cap, one number.
    class QLabel* m_levelLabel{nullptr};
    class QLabel* m_endLabel{nullptr};   // the "OUT" cap
    Qt::Orientation m_orientation{Qt::Vertical};

    // Peak / trough sweep markers, the same tracker the VFO flag's meter uses.
    // Driven from setPeakLinear(), which the panel's FFT timer already calls at
    // ~25 Hz, so this needs no clock of its own beyond measuring the gaps.
    MeterExtremes m_extremes;
    QElapsedTimer m_extremesClock;
    qint64        m_lastExtremesMs{0};
    float   m_gain{1.0f};
    bool    m_gainControl{true};
    float   m_smoothedPeak{-120.0f};  // dB
    bool    m_dragging{false};

    // dB ranges
    static constexpr float kGainMinDb  = -36.0f;
    static constexpr float kGainMaxDb  = +12.0f;
    static constexpr float kMeterMinDb = -60.0f;
    // Above 0, not at it: the EQ can add gain and the operator needs to see how
    // far past unity the output has gone, not just that it is pegged.
    static constexpr float kMeterMaxDb = +12.0f;

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
    // so they don't recompute geometry on every move. Origin and length are
    // along the strip's own axis, whichever that is.
    int m_stripOrigin{0};
    int m_stripLength{0};
};

} // namespace AetherSDR
