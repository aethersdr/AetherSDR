#include "PhaseKnob.h"

#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr float kMargin = 4.0f;

// Painted radius in pixels for the current widget size.
double paintedRadius(const QSize& size)
{
    const int side = std::min(size.width(), size.height());
    return (side - 2.0 * kMargin) / 2.0;
}

} // namespace

PhaseKnob::PhaseKnob(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(120, 120);
    setCursor(Qt::CrossCursor);
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

void PhaseKnob::deltaToPolar(double dx, double dy, double maxRadius,
                             float& outPhase, float& outGain)
{
    // Paint mapping (see paintEvent below):
    //   angle = phase - π/2
    //   dot   = center + gain/2 * radius * (cos(angle), sin(angle))
    // Inverse:
    //   phase = atan2(dy, dx) + π/2   (normalized to [0, 2π))
    //   gain  = clamp(r / maxRadius, 0, 1) * 2.0
    const double r = std::sqrt(dx * dx + dy * dy);
    double phase = std::atan2(dy, dx) + M_PI / 2.0;
    if (phase < 0.0) phase += 2.0 * M_PI;
    if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
    const double gain = (maxRadius > 0.0)
        ? std::clamp(r / maxRadius, 0.0, 1.0) * 2.0
        : 0.0;
    outPhase = static_cast<float>(phase);
    outGain  = static_cast<float>(gain);
}

void PhaseKnob::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    updateFromMouse(event->position());
    event->accept();
}

void PhaseKnob::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    updateFromMouse(event->position());
    event->accept();
}

void PhaseKnob::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    event->accept();
}

void PhaseKnob::updateFromMouse(const QPointF& pos)
{
    const QPointF center(width() / 2.0, height() / 2.0);
    const double dx = pos.x() - center.x();
    const double dy = pos.y() - center.y();
    const double maxR = paintedRadius(size());

    float phase = 0.0f;
    float gain  = 0.0f;
    deltaToPolar(dx, dy, maxR, phase, gain);

    if (!qFuzzyCompare(m_phase, phase)) {
        m_phase = phase;
        emit phaseChanged(phase);
    }
    if (!qFuzzyCompare(m_gain, gain)) {
        m_gain = gain;
        emit gainChanged(gain);
    }
    update();
}

void PhaseKnob::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const float radius = static_cast<float>(paintedRadius(size()));
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

    // Dot — slightly larger while dragging for affordance
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x00, 0xb4, 0xd8));
    const float dotSize = m_dragging ? 5.0f : 4.0f;
    p.drawEllipse(QPointF(dotX, dotY), dotSize, dotSize);

    // Center dot
    p.setBrush(QColor(0x50, 0x60, 0x70));
    p.drawEllipse(center, 2, 2);
}

} // namespace AetherSDR
