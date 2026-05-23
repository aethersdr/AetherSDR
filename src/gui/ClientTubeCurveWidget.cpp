#include "ClientTubeCurveWidget.h"
#include "core/ClientTube.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QRadialGradient>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr float kBallSmoothAlpha = 0.30f;

const QColor kBgColor       ("#0a1420");
const QColor kFrameColor    ("#2a3a4a");
const QColor kGridColor     ("#1e3040");
const QColor kAxisColor     ("#2a4458");
const QColor kCurveColor    ("#4db8d4");   // cyan
const QColor kBallGlowColor ("#ffd070");
const QColor kBallCoreColor ("#ffffff");

float dbToLin(float db) noexcept
{
    return std::pow(10.0f, db * 0.05f);
}

} // namespace

ClientTubeCurveWidget::ClientTubeCurveWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(80);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(33);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        if (!m_tube) return;
        // Convert peak dB to linear amplitude for ball placement.
        const float targetLin = dbToLin(m_tube->inputPeakDb());
        m_lastInputLin += kBallSmoothAlpha * (targetLin - m_lastInputLin);
        update();
    });
}

void ClientTubeCurveWidget::setTube(ClientTube* t)
{
    m_tube = t;
    if (m_tube) m_pollTimer->start();
    else        m_pollTimer->stop();
    update();
}

void ClientTubeCurveWidget::setCompactMode(bool on)
{
    if (on == m_compact) return;
    m_compact = on;
    update();
}

float ClientTubeCurveWidget::xToPx(float x) const
{
    const float t = (x + kAxisLimit) / (2.0f * kAxisLimit);
    return static_cast<float>(rect().left()) + t * rect().width();
}

float ClientTubeCurveWidget::yToPx(float y) const
{
    const float t = (y + kAxisLimit) / (2.0f * kAxisLimit);
    return static_cast<float>(rect().bottom()) - t * rect().height();
}

float ClientTubeCurveWidget::evalCurve(float x) const
{
    if (!m_tube) return x;
    const float drive = dbToLin(m_tube->driveDb());
    const float bias  = m_tube->biasAmount();
    const int   model = static_cast<int>(m_tube->model());
    const float xd    = x * drive;
    const float asym  = bias * std::tanh(xd * xd);

    switch (model) {
        case 0:   // Model A — soft tanh
            return std::tanh(xd) + asym;
        case 1: { // Model B — hard clip + tanh hybrid
            const float h = std::clamp(xd, -1.0f, 1.0f);
            return std::tanh(h * 1.3f) * 0.85f + asym;
        }
        case 2: { // Model C — asymmetric-dominant
            const float t  = std::tanh(xd);
            const float a2 = std::tanh(xd * xd);
            return 0.75f * t + (0.35f + 0.65f * bias) * a2;
        }
    }
    return std::tanh(xd);
}

void ClientTubeCurveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect();
    p.fillRect(r, kBgColor);

    // Grid — a few reference lines at ±0.5, ±1.0.
    p.setPen(QPen(kGridColor, 1.0));
    for (float v : { -1.0f, -0.5f, 0.5f, 1.0f }) {
        p.drawLine(QPointF(xToPx(v), r.top()), QPointF(xToPx(v), r.bottom()));
        p.drawLine(QPointF(r.left(), yToPx(v)), QPointF(r.right(), yToPx(v)));
    }

    // Centre axes — slightly brighter.
    p.setPen(QPen(kAxisColor, 1.0));
    p.drawLine(QPointF(xToPx(0.0f), r.top()),  QPointF(xToPx(0.0f), r.bottom()));
    p.drawLine(QPointF(r.left(), yToPx(0.0f)), QPointF(r.right(), yToPx(0.0f)));

    // Transfer curve.
    if (m_tube) {
        QPainterPath path;
        constexpr int steps = 161;
        for (int i = 0; i < steps; ++i) {
            const float t = static_cast<float>(i) / (steps - 1);
            const float x = -kAxisLimit + t * (2.0f * kAxisLimit);
            const float y = std::clamp(evalCurve(x), -kAxisLimit, kAxisLimit);
            const QPointF pt(xToPx(x), yToPx(y));
            if (i == 0) path.moveTo(pt);
            else        path.lineTo(pt);
        }
        QPen curvePen(kCurveColor, m_compact ? 1.5 : 2.0);
        curvePen.setJoinStyle(Qt::RoundJoin);
        curvePen.setCapStyle(Qt::RoundCap);
        p.setPen(curvePen);
        p.drawPath(path);

        // Live ball at (input, curveOutput(input)).
        const float x  = std::clamp(m_lastInputLin, -kAxisLimit, kAxisLimit);
        const float y  = std::clamp(evalCurve(x), -kAxisLimit, kAxisLimit);
        const QPointF pt(xToPx(x), yToPx(y));
        const float glow = m_compact ? 7.0f : 10.0f;
        QRadialGradient g(pt, glow);
        QColor gc = kBallGlowColor; gc.setAlpha(200);
        g.setColorAt(0.0, gc);
        gc.setAlpha(0);
        g.setColorAt(1.0, gc);
        p.setBrush(g);
        p.setPen(Qt::NoPen);
        p.drawEllipse(pt, glow, glow);
        p.setBrush(kBallCoreColor);
        p.drawEllipse(pt, m_compact ? 2.2 : 3.0,
                          m_compact ? 2.2 : 3.0);
    }

    // Frame.
    p.setPen(QPen(kFrameColor, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
}

} // namespace MasterSDR
