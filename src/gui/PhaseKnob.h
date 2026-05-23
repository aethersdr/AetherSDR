#pragma once

#include <QWidget>

namespace MasterSDR {

// Polar display for ESC (Enhanced Signal Clarity).
// Shows a crosshair circle with a dot positioned by phase (angle) and
// gain (distance from center). Phase and gain are set externally via
// sliders — this widget is display-only.
class PhaseKnob : public QWidget {
    Q_OBJECT

public:
    explicit PhaseKnob(QWidget* parent = nullptr);

    // Phase in radians (0 – 2π), gain 0.0 – 2.0
    void setPhase(float radians);
    void setGain(float gain);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    float m_phase{0.0f};   // radians
    float m_gain{1.0f};    // 0.0 – 2.0 (1.0 = half radius)
};

} // namespace MasterSDR
