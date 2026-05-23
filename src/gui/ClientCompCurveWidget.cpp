#include "ClientCompCurveWidget.h"
#include "core/ClientComp.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr float kBallSmoothAlpha = 0.30f;  // per-tick; ~8 ticks to settle

const QColor kBgColor       ("#0a1420");
const QColor kGridColor     ("#1e3040");
const QColor kGridMajorColor("#2a4458");
const QColor kAxisLabelColor("#607888");
const QColor kIdentityColor ("#2a3a4a");
const QColor kCurveColor    ("#4db8d4");
const QColor kBallGlowColor ("#ffd070");
const QColor kBallCoreColor ("#ffffff");

// dBFS ticks for the grid (both axes).
const float kMajorTicks[] = { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f };
const float kMinorTicks[] = { -6.0f, -18.0f, -30.0f, -42.0f, -54.0f };

} // namespace

ClientCompCurveWidget::ClientCompCurveWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(80);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(33);  // ~30 Hz, plenty for a sliding ball
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        if (!m_comp) return;
        const float target = m_comp->inputPeakDb();
        // One-pole smoother keeps the ball from twitching on silent frames
        // where the peak meter reads -120 dBFS; on real signal it tracks
        // within a few ticks.
        m_lastInputDb += kBallSmoothAlpha * (target - m_lastInputDb);
        update();
    });
}

void ClientCompCurveWidget::setComp(ClientComp* comp)
{
    m_comp = comp;
    if (m_comp) m_pollTimer->start();
    else        m_pollTimer->stop();
    update();
}

void ClientCompCurveWidget::setCompactMode(bool on)
{
    if (on == m_compact) return;
    m_compact = on;
    m_labelsDirty = true;  // font px size flips 9 ↔ 7
    update();
}

float ClientCompCurveWidget::dbToX(float db) const
{
    const float t = (db - kMinDb) / (kMaxDb - kMinDb);
    return static_cast<float>(rect().left()) + t * rect().width();
}

float ClientCompCurveWidget::dbToY(float db) const
{
    const float t = (db - kMinDb) / (kMaxDb - kMinDb);
    return static_cast<float>(rect().bottom()) - t * rect().height();
}

float ClientCompCurveWidget::xToDb(float x) const
{
    const float t = (x - rect().left()) / std::max(1, rect().width());
    return kMinDb + t * (kMaxDb - kMinDb);
}

float ClientCompCurveWidget::yToDb(float y) const
{
    const float t = (rect().bottom() - y) / std::max(1, rect().height());
    return kMinDb + t * (kMaxDb - kMinDb);
}

float ClientCompCurveWidget::curveOutputDb(float inDb) const
{
    if (!m_comp) return inDb;
    const float T     = m_comp->thresholdDb();
    const float W     = m_comp->kneeDb();
    const float ratio = std::max(1.0f, m_comp->ratio());
    const float slope = 1.0f - 1.0f / ratio;   // 0 = unity, 1 = limit
    const float over  = inDb - T;

    float gain;
    if (W <= 0.0f) {
        gain = (over > 0.0f) ? -over * slope : 0.0f;
    } else if (over <= -0.5f * W) {
        gain = 0.0f;
    } else if (over >= 0.5f * W) {
        gain = -over * slope;
    } else {
        const float x = over + 0.5f * W;
        gain = -slope * (x * x) / (2.0f * W);
    }
    return inDb + gain + m_comp->makeupDb();
}

void ClientCompCurveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect();
    p.fillRect(r, kBgColor);
    drawGrid(p, r);
    drawCurve(p, r);
    if (m_comp) drawBall(p, r);
}

void ClientCompCurveWidget::drawGrid(QPainter& p, const QRectF& r) const
{
    p.save();

    const QPen minorPen(kGridColor, 1.0);
    const QPen majorPen(kGridMajorColor, 1.0);

    // Minor ticks
    p.setPen(minorPen);
    for (float db : kMinorTicks) {
        const float x = dbToX(db);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        const float y = dbToY(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }

    // Major ticks — heavier lines; labels too (unless compact).
    p.setPen(majorPen);
    QFont f = p.font();
    f.setPixelSize(m_compact ? 7 : 9);
    p.setFont(f);

    // Rebuild the QStaticText cache on first paint or after a compact-
    // mode flip — strings come from kMajorTicks (compile-time constant),
    // so the HarfBuzz shape pass runs once per (label, font) pair.
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

    // drawStaticText interprets QPointF as the top-left of the text rect
    // (drawText interprets it as the baseline) — subtract the font ascent
    // from the y-coord to preserve the prior visual position.
    const qreal ascent = p.fontMetrics().ascent();

    for (size_t i = 0; i < std::size(kMajorTicks); ++i) {
        const float db = kMajorTicks[i];
        const float x = dbToX(db);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        const float y = dbToY(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));

        if (!m_compact && db != 0.0f && db != kMinDb) {
            // prepare() with the painter's actual transform avoids a
            // first-paint rebuild on HiDPI displays.
            m_axisLabels[i].prepare(p.transform(), f);
            p.setPen(kAxisLabelColor);
            p.drawStaticText(QPointF(x + 2.0f, r.bottom() - 2.0f - ascent),
                             m_axisLabels[i]);
            p.drawStaticText(QPointF(r.left() + 2.0f, y - 2.0f - ascent),
                             m_axisLabels[i]);
            p.setPen(majorPen);
        }
    }

    // Unity diagonal (below-threshold passthrough) drawn as a dim
    // reference so the user can see how far the curve bends away.
    p.setPen(QPen(kIdentityColor, 1.0, Qt::DashLine));
    p.drawLine(QPointF(dbToX(kMinDb), dbToY(kMinDb)),
               QPointF(dbToX(kMaxDb), dbToY(kMaxDb)));

    p.restore();
}

void ClientCompCurveWidget::drawCurve(QPainter& p, const QRectF& r) const
{
    if (!m_comp) return;

    p.save();

    // Walk across the input range in ~1 dB steps — the knee transition
    // is smooth enough that this is plenty, and it keeps the path short.
    QPainterPath path;
    constexpr int steps = 121;                     // 60 dB range, 0.5 dB/step
    for (int i = 0; i < steps; ++i) {
        const float t    = static_cast<float>(i) / (steps - 1);
        const float inDb = kMinDb + t * (kMaxDb - kMinDb);
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

    // Threshold tick on the curve — small filled dot at the knee centre
    // so the user's eye finds the threshold point even at a glance.
    if (!m_compact) {
        const float T = m_comp->thresholdDb();
        const QPointF t(dbToX(T), dbToY(curveOutputDb(T)));
        p.setBrush(kCurveColor);
        p.setPen(Qt::NoPen);
        p.drawEllipse(t, 3.0, 3.0);
    }

    p.restore();
}

void ClientCompCurveWidget::drawBall(QPainter& p, const QRectF& r) const
{
    if (!m_comp) return;
    const float inDb = std::clamp(m_lastInputDb, kMinDb, kMaxDb);
    const float outDb = std::clamp(curveOutputDb(inDb), kMinDb, kMaxDb);
    const QPointF pt(dbToX(inDb), dbToY(outDb));

    p.save();
    // Glow halo — radial gradient so the ball reads as a light source
    // even at low GR.  Size scales slightly with input level so busy
    // passages look livelier than quiet ones.
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

    // White core
    p.setBrush(kBallCoreColor);
    p.drawEllipse(pt, m_compact ? 2.5 : 3.5, m_compact ? 2.5 : 3.5);
    p.restore();
}

} // namespace MasterSDR
