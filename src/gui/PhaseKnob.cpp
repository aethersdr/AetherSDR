#include "PhaseKnob.h"

#include <QPainter>
#include <cmath>

namespace MasterSDR {

PhaseKnob::PhaseKnob(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(120, 120);
}

void PhaseKnob::setPhase(float radians)
{
    if (qFuzzyCompare(m_phase, radians)) return;
    m_phase = radians;
    update();
}

void PhaseKnob::setGain(float gain)
{
    if (qFuzzyCompare(m_gain, gain)) return;
    m_gain = gain;
    update();
}

void PhaseKnob::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int side = std::min(width(), height());
    constexpr float margin = 4.0f;
    const float radius = (side - 2 * margin) / 2.0f;
    const QPointF center(width() / 2.0, height() / 2.0);

    // Dark background circle
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x10, 0x10, 0x1c));
    p.drawEllipse(center, radius, radius);

    // Outer ring
    p.setPen(QPen(QColor(0x40, 0x50, 0x60), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(center, radius, radius);

    // Crosshair lines (vertical + horizontal through center)
    p.setPen(QPen(QColor(0x30, 0x40, 0x50), 0.5));
    p.drawLine(QPointF(center.x(), center.y() - radius),
               QPointF(center.x(), center.y() + radius));
    p.drawLine(QPointF(center.x() - radius, center.y()),
               QPointF(center.x() + radius, center.y()));

    // Mid-radius ring (gain = 1.0 reference)
    p.setPen(QPen(QColor(0x30, 0x40, 0x50), 0.5, Qt::DotLine));
    p.drawEllipse(center, radius * 0.5f, radius * 0.5f);

    // Dot position: phase sets angle (0 = top/12 o'clock), gain sets distance
    // gain 0.0 = center, gain 2.0 = edge
    const float dotRadius = radius * (m_gain / 2.0f);
    const double angle = m_phase - M_PI / 2.0;  // -π/2 so 0 rad = top
    const float dotX = center.x() + dotRadius * std::cos(angle);
    const float dotY = center.y() + dotRadius * std::sin(angle);

    // Faint line from center to dot
    p.setPen(QPen(QColor(0x00, 0xb4, 0xd8, 60), 1.0));
    p.drawLine(center, QPointF(dotX, dotY));

    // Dot
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x00, 0xb4, 0xd8));
    p.drawEllipse(QPointF(dotX, dotY), 4, 4);

    // Center dot
    p.setBrush(QColor(0x50, 0x60, 0x70));
    p.drawEllipse(center, 2, 2);
}

} // namespace MasterSDR
