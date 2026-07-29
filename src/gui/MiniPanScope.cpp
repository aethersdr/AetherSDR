#include "gui/MiniPanScope.h"

#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <algorithm>

namespace AetherSDR {

MiniPanScope::MiniPanScope(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumHeight(90);
}

void MiniPanScope::updateSpectrum(const QVector<float>& binsDbm)
{
    m_bins = binsDbm;
    update();
}

void MiniPanScope::setDbmRange(float minDbm, float maxDbm)
{
    m_minDbm = minDbm;
    m_maxDbm = maxDbm;
    update();
}

void MiniPanScope::setSpanKHz(double kHz)
{
    m_spanKHz = kHz > 0.0 ? kHz : m_spanKHz;
    update();
}

void MiniPanScope::setPassbandHz(int lowHz, int highHz)
{
    m_pbLoHz = lowHz;
    m_pbHiHz = highHz;
    update();
}

void MiniPanScope::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const double w = width(), h = height();
    p.fillRect(rect(), QColor(0x0a, 0x16, 0x2c));

    const double halfHz = m_spanKHz * 1000.0 / 2.0;   // e.g. 5000 Hz for ±5 kHz
    const auto xOf = [&](double offHz) {
        return w * 0.5 + (offHz / halfHz) * (w * 0.5);
    };

    // Passband band (translucent, brighter than the field).
    if (m_pbHiHz > m_pbLoHz) {
        const double x0 = std::clamp(xOf(m_pbLoHz), 0.0, w);
        const double x1 = std::clamp(xOf(m_pbHiHz), 0.0, w);
        p.fillRect(QRectF(x0, 0, x1 - x0, h), QColor(60, 96, 150, 90));
    }

    // Faint dB grid.
    p.setPen(QColor(255, 255, 255, 18));
    for (int i = 1; i < 4; ++i)
        p.drawLine(QPointF(0, h * i / 4.0), QPointF(w, h * i / 4.0));

    // Centre hairline.
    p.setPen(QColor(150, 180, 220, 120));
    p.drawLine(QPointF(w * 0.5, 0), QPointF(w * 0.5, h));

    // FFT trace: filled polygon + bright line.
    const int n = m_bins.size();
    if (n >= 2) {
        const double span = std::max(1.0f, m_maxDbm - m_minDbm);
        const auto yOf = [&](float dbm) {
            const double t = (m_maxDbm - dbm) / span;   // 0 at top (max), 1 at bottom
            return std::clamp(t, 0.0, 1.0) * h;
        };
        QPainterPath line;
        for (int i = 0; i < n; ++i) {
            const double x = (double(i) / (n - 1)) * w;
            const double y = yOf(m_bins[i]);
            if (i == 0) line.moveTo(x, y);
            else        line.lineTo(x, y);
        }
        QPainterPath fill = line;
        fill.lineTo(w, h);
        fill.lineTo(0, h);
        fill.closeSubpath();
        p.fillPath(fill, QColor(220, 200, 40, 60));
        p.setPen(QPen(QColor(240, 220, 60), 1.2));
        p.drawPath(line);
    }

    // ±span corner labels (K4 style).
    QFont f = p.font();
    f.setPointSizeF(9.0);
    p.setFont(f);
    p.setPen(QColor(150, 190, 230));
    const QString half = QString::number(m_spanKHz / 2.0, 'f', 1);
    p.drawText(QRectF(4, 2, w / 2 - 6, 16), Qt::AlignLeft  | Qt::AlignVCenter,
               "-" + half + " kHz");
    p.drawText(QRectF(w / 2 + 2, 2, w / 2 - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
               half + " kHz");
}

} // namespace AetherSDR
