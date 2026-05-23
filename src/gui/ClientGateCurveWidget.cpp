#include "ClientGateCurveWidget.h"
#include "core/ClientGate.h"

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
const QColor kGridColor     ("#1e3040");
const QColor kGridMajorColor("#2a4458");
const QColor kAxisLabelColor("#607888");
const QColor kIdentityColor ("#2a3a4a");
const QColor kCurveColor    ("#c8a040");   // amber — reads as gate/expander
const QColor kBallGlowColor ("#ffd070");
const QColor kBallCoreColor ("#ffffff");

// -80..0 dB range with matching majors; gate can attenuate much
// deeper than comp, so the grid needs to show the full -80 floor.
const float kMajorTicks[] = { 0.0f, -20.0f, -40.0f, -60.0f, -80.0f };
const float kMinorTicks[] = { -10.0f, -30.0f, -50.0f, -70.0f };

} // namespace

ClientGateCurveWidget::ClientGateCurveWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(80);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(33);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        if (!m_gate) return;
        const float target = m_gate->inputPeakDb();
        m_lastInputDb += kBallSmoothAlpha * (target - m_lastInputDb);
        update();
    });
}

void ClientGateCurveWidget::setGate(ClientGate* gate)
{
    m_gate = gate;
    if (m_gate) m_pollTimer->start();
    else        m_pollTimer->stop();
    update();
}

void ClientGateCurveWidget::setCompactMode(bool on)
{
    if (on == m_compact) return;
    m_compact = on;
    m_labelsDirty = true;  // font px size flips 9 ↔ 7
    update();
}

float ClientGateCurveWidget::dbToX(float db) const
{
    const float t = (db - kMinDb) / (kMaxDb - kMinDb);
    return static_cast<float>(rect().left()) + t * rect().width();
}

float ClientGateCurveWidget::dbToY(float db) const
{
    const float t = (db - kMinDb) / (kMaxDb - kMinDb);
    return static_cast<float>(rect().bottom()) - t * rect().height();
}

float ClientGateCurveWidget::curveOutputDb(float inDb) const
{
    if (!m_gate) return inDb;
    // Downward expander static curve:
    //   shortfall = T - in
    //   if shortfall <= 0  → gain = 0 (unity, above threshold)
    //   else                 gain = max(-shortfall * (ratio-1), floor)
    const float T         = m_gate->thresholdDb();
    const float ratio     = std::max(1.0f, m_gate->ratio());
    const float floorDb   = m_gate->floorDb();
    const float slope     = ratio - 1.0f;
    const float shortfall = T - inDb;
    float gain = 0.0f;
    if (shortfall > 0.0f) gain = std::max(-shortfall * slope, floorDb);
    return inDb + gain;
}

void ClientGateCurveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect();
    p.fillRect(r, kBgColor);
    drawGrid(p, r);
    drawHysteresisBand(p, r);
    drawCurve(p, r);
    if (m_gate) drawBall(p, r);
}

void ClientGateCurveWidget::drawHysteresisBand(QPainter& p, const QRectF& r) const
{
    // Visualises the Return knob: a vertical band on the input axis
    // between (Thresh − Return) and Thresh.  When the input envelope
    // (the glowing ball) sits inside this band the gate's state is
    // "sticky" — opens above Thresh, doesn't close until below
    // Thresh − Return.  Width of the band == Return value in dB.
    if (!m_gate) return;
    const float T   = m_gate->thresholdDb();
    const float ret = m_gate->returnDb();
    if (ret <= 0.05f) return;   // no visible deadband

    const float xR = dbToX(T);
    const float xL = dbToX(T - ret);
    if (xR <= xL) return;

    p.save();
    p.setPen(Qt::NoPen);
    p.fillRect(QRectF(xL, r.top(), xR - xL, r.height()),
               QColor(80, 180, 220, 45));   // soft cyan, low alpha
    p.restore();
}

void ClientGateCurveWidget::drawGrid(QPainter& p, const QRectF& r) const
{
    p.save();

    const QPen minorPen(kGridColor, 1.0);
    const QPen majorPen(kGridMajorColor, 1.0);

    p.setPen(minorPen);
    for (float db : kMinorTicks) {
        const float x = dbToX(db);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        const float y = dbToY(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }

    p.setPen(majorPen);
    QFont f = p.font();
    f.setPixelSize(m_compact ? 7 : 9);
    p.setFont(f);

    if (m_labelsDirty) {
        m_axisLabels.clear();
        m_axisLabels.reserve(static_cast<int>(std::size(kMajorTicks)));
        for (float db : kMajorTicks) {
            QStaticText st(QString::number(static_cast<int>(db)));
            st.setPerformanceHint(QStaticText::AggressiveCaching);
            m_axisLabels.append(std::move(st));
        }
        m_labelsDirty = false;
    }

    const qreal ascent = p.fontMetrics().ascent();

    for (size_t i = 0; i < std::size(kMajorTicks); ++i) {
        const float db = kMajorTicks[i];
        const float x = dbToX(db);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        const float y = dbToY(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));

        if (!m_compact && db != 0.0f && db != kMinDb) {
            m_axisLabels[i].prepare(p.transform(), f);
            p.setPen(kAxisLabelColor);
            p.drawStaticText(QPointF(x + 2.0f, r.bottom() - 2.0f - ascent),
                             m_axisLabels[i]);
            p.drawStaticText(QPointF(r.left() + 2.0f, y - 2.0f - ascent),
                             m_axisLabels[i]);
            p.setPen(majorPen);
        }
    }

    // Identity diagonal — dim reference showing where the curve sits
    // when the gate is fully open (above threshold).
    p.setPen(QPen(kIdentityColor, 1.0, Qt::DashLine));
    p.drawLine(QPointF(dbToX(kMinDb), dbToY(kMinDb)),
               QPointF(dbToX(kMaxDb), dbToY(kMaxDb)));

    p.restore();
}

void ClientGateCurveWidget::drawCurve(QPainter& p, const QRectF& r) const
{
    if (!m_gate) return;

    p.save();

    QPainterPath path;
    constexpr int steps = 161;                     // 80 dB range, 0.5 dB/step
    for (int i = 0; i < steps; ++i) {
        const float t     = static_cast<float>(i) / (steps - 1);
        const float inDb  = kMinDb + t * (kMaxDb - kMinDb);
        const float outDb = std::clamp(curveOutputDb(inDb), kMinDb, kMaxDb);
        const QPointF pt(dbToX(inDb), dbToY(outDb));
        if (i == 0) path.moveTo(pt);
        else        path.lineTo(pt);
    }

    QPen curvePen(kCurveColor, m_compact ? 1.5 : 2.0);
    curvePen.setJoinStyle(Qt::RoundJoin);
    curvePen.setCapStyle(Qt::RoundCap);
    p.setPen(curvePen);
    p.drawPath(path);

    // Threshold dot at the knee corner so the user's eye locks on it.
    if (!m_compact) {
        const float T = m_gate->thresholdDb();
        const QPointF t(dbToX(T), dbToY(curveOutputDb(T)));
        p.setBrush(kCurveColor);
        p.setPen(Qt::NoPen);
        p.drawEllipse(t, 3.0, 3.0);
    }

    p.restore();
}

void ClientGateCurveWidget::drawBall(QPainter& p, const QRectF& r) const
{
    if (!m_gate) return;
    const float inDb  = std::clamp(m_lastInputDb, kMinDb, kMaxDb);
    const float outDb = std::clamp(curveOutputDb(inDb), kMinDb, kMaxDb);
    const QPointF pt(dbToX(inDb), dbToY(outDb));

    p.save();
    const float glow = m_compact ? 8.0f : 11.0f;
    QRadialGradient g(pt, glow);
    QColor glowColor = kBallGlowColor;
    glowColor.setAlpha(200);
    g.setColorAt(0.0, glowColor);
    glowColor.setAlpha(0);
    g.setColorAt(1.0, glowColor);
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawEllipse(pt, glow, glow);

    p.setBrush(kBallCoreColor);
    p.drawEllipse(pt, m_compact ? 2.5 : 3.5, m_compact ? 2.5 : 3.5);
    p.restore();
}

} // namespace MasterSDR
