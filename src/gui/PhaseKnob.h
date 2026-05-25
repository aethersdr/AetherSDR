#pragma once

#include <QWidget>

class QMouseEvent;

namespace AetherSDR {

// Polar display for ESC (Enhanced Signal Clarity).
// Shows a crosshair circle with a dot positioned by phase (angle) and
// gain (distance from center).  Clicking or dragging inside the circle
// emits phaseChanged/gainChanged so the parent can drive the model and
// keep the phase/gain sliders in sync.
class PhaseKnob : public QWidget {
    Q_OBJECT

public:
    explicit PhaseKnob(QWidget* parent = nullptr);

    // Phase in radians (0 – 2π), gain 0.0 – 2.0
    void setPhase(float radians);
    void setGain(float gain);

    // Static coordinate conversion (exposed for unit tests).
    // `delta` is the click position relative to the circle center;
    // `maxRadius` is the painted radius in pixels.  Output phase is in
    // radians normalized to [0, 2π); output gain is in [0, 2].
    static void deltaToPolar(double dx, double dy, double maxRadius,
                             float& outPhase, float& outGain);

signals:
    void phaseChanged(float radians);
    void gainChanged(float gain);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void updateFromMouse(const QPointF& pos);

    float m_phase{0.0f};   // radians
    float m_gain{1.0f};    // 0.0 – 2.0 (1.0 = half radius)
    bool  m_dragging{false};
};

} // namespace AetherSDR
