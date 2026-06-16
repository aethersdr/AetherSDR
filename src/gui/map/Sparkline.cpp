#include "Sparkline.h"

#include <QPainter>
#include <QPainterPath>
#include <QPalette>

#include <algorithm>

namespace AetherSDR {

namespace {
constexpr int kWidth = 120;
constexpr int kHeight = 20;
} // namespace

Sparkline::Sparkline(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(kWidth, kHeight);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void Sparkline::setCapacity(int n)
{
    m_capacity = std::max(2, n);
    while (m_samples.size() > m_capacity) {
        m_samples.removeFirst();
    }
    update();
}

void Sparkline::addSample(double value)
{
    m_samples.append(value);
    while (m_samples.size() > m_capacity) {
        m_samples.removeFirst();
    }
    update();
}

void Sparkline::setSamples(const QVector<double>& s)
{
    m_samples = s;
    while (m_samples.size() > m_capacity) {
        m_samples.removeFirst();
    }
    update();
}

void Sparkline::clear()
{
    m_samples.clear();
    update();
}

void Sparkline::paintEvent(QPaintEvent*)
{
    if (m_samples.size() < 2) {
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const double maxV = *std::max_element(m_samples.begin(), m_samples.end());
    const double range = maxV > 0.0 ? maxV : 1.0;  // baseline at 0
    const double w = width() - 1.0;
    const double h = height() - 3.0;
    const double dx = w / (m_samples.size() - 1);

    QPainterPath path;
    for (int i = 0; i < m_samples.size(); ++i) {
        const double x = i * dx;
        const double y = 1.0 + (h - (m_samples[i] / range) * h);
        if (i == 0) {
            path.moveTo(x, y);
        } else {
            path.lineTo(x, y);
        }
    }

    // Soft filled area under the line, then the line itself.
    const QColor accent = palette().color(QPalette::Highlight);
    QPainterPath fill = path;
    fill.lineTo(w, height());
    fill.lineTo(0, height());
    fill.closeSubpath();
    QColor fillColor = accent;
    fillColor.setAlpha(40);
    p.fillPath(fill, fillColor);

    p.setPen(QPen(accent, 1.5));
    p.drawPath(path);

    // Highlight the most recent point.
    const double lastX = (m_samples.size() - 1) * dx;
    const double lastY = 1.0 + (h - (m_samples.last() / range) * h);
    p.setBrush(accent);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(lastX, lastY), 2.0, 2.0);
}

QSize Sparkline::sizeHint() const
{
    return { kWidth, kHeight };
}

} // namespace AetherSDR
