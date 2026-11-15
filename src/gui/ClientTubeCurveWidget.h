#pragma once

#include <QWidget>

class QTimer;

namespace AetherSDR {

class ClientTube;

// Transfer-curve display for the tube saturator.  X axis is input
// amplitude in [-1.5, +1.5] (so the curve shows how the shaper
// behaves above full scale as well as below); Y axis is output
// amplitude over the same range.  The curve bends according to the
// current Model / Drive / Bias values.  A live ball tracks the
// instantaneous input amplitude along the curve so the user can see
// which part of the transfer function the signal is riding on.
class ClientTubeCurveWidget : public QWidget {
    Q_OBJECT

public:
    explicit ClientTubeCurveWidget(QWidget* parent = nullptr);

    void setTube(ClientTube* t);
    ClientTube* tube() const { return m_tube; }

    void setCompactMode(bool on);

    static constexpr float kAxisLimit = 1.5f;   // display range on both axes

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    float xToPx(float x) const;
    float yToPx(float y) const;
    // Evaluate the current model's transfer function at input x.
    // Mirrors ClientTube::shape() math but takes only the user's
    // current Drive + Bias into account (no envelope modulation —
    // the curve shows the static response).
    float evalCurve(float x) const;

    ClientTube* m_tube{nullptr};
    QTimer*     m_pollTimer{nullptr};
    bool        m_compact{false};
    float       m_lastInputLin{0.0f};   // smoothed ball position
};

} // namespace AetherSDR
