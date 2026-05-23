#include "ClientDeEssCurveWidget.h"
#include "core/ClientDeEss.h"

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
constexpr float kTwoPi = 6.283185307179586476f;

const QColor kBgColor       ("#0a1420");
const QColor kGridColor     ("#1e3040");
const QColor kGridMajorColor("#2a4458");
const QColor kAxisLabelColor("#607888");
const QColor kCurveColor    ("#d47272");   // soft red — "sibilant band"
const QColor kThreshColor   ("#4db8d4");
const QColor kBallGlowColor ("#ffd070");
const QColor kBallCoreColor ("#ffffff");

// Log-freq major ticks.
const float kFreqMajor[] = { 100.0f, 500.0f, 1000.0f, 2000.0f,
                             5000.0f, 10000.0f };

} // namespace

ClientDeEssCurveWidget::ClientDeEssCurveWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(80);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(33);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        if (!m_deEss) return;
        const float target = m_deEss->sidechainPeakDb();
        m_lastScDb += kBallSmoothAlpha * (target - m_lastScDb);
        update();
    });
}

void ClientDeEssCurveWidget::setDeEss(ClientDeEss* d)
{
    m_deEss = d;
    if (m_deEss) m_pollTimer->start();
    else         m_pollTimer->stop();
    update();
}

void ClientDeEssCurveWidget::setCompactMode(bool on)
{
    if (on == m_compact) return;
    m_compact = on;
    update();
}

float ClientDeEssCurveWidget::hzToX(float hz) const
{
    const float lhz = std::log10(std::max(hz, 1.0f));
    const float lmin = std::log10(kMinHz);
    const float lmax = std::log10(kMaxHz);
    const float t = (lhz - lmin) / (lmax - lmin);
    return static_cast<float>(rect().left()) + t * rect().width();
}

float ClientDeEssCurveWidget::dbToY(float db) const
{
    const float t = (db - kMinDb) / (kMaxDb - kMinDb);
    return static_cast<float>(rect().bottom()) - t * rect().height();
}

float ClientDeEssCurveWidget::bandpassMagDb(float hz) const
{
    if (!m_deEss || m_deEss->sampleRate() <= 0.0) return kMinDb;
    const float fc = m_deEss->frequencyHz();
    const float q  = std::max(0.1f, m_deEss->q());
    const float fs = static_cast<float>(m_deEss->sampleRate());

    // RBJ bandpass (constant 0 dB peak) — same coefficients as the DSP.
    const float omega = kTwoPi * fc / fs;
    const float sinw  = std::sin(omega);
    const float cosw  = std::cos(omega);
    const float alpha = sinw / (2.0f * q);

    const float a0 = 1.0f + alpha;
    const float b0 =  alpha         / a0;
    const float b1 =  0.0f;
    const float b2 = -alpha         / a0;
    const float a1n = (-2.0f * cosw) / a0;
    const float a2n = (1.0f - alpha) / a0;

    const float w = kTwoPi * hz / fs;
    // |H(e^jw)|^2 = |B|^2 / |A|^2.
    // B = b0 + b1 e^-jw + b2 e^-j2w; A = 1 + a1 e^-jw + a2 e^-j2w.
    const float cw = std::cos(w);
    const float sw = std::sin(w);
    const float c2w = std::cos(2.0f * w);
    const float s2w = std::sin(2.0f * w);

    const float bReal = b0 + b1 * cw + b2 * c2w;
    const float bImag =     -b1 * sw - b2 * s2w;
    const float aReal = 1.0f + a1n * cw + a2n * c2w;
    const float aImag =      -a1n * sw - a2n * s2w;

    const float bMag2 = bReal * bReal + bImag * bImag;
    const float aMag2 = aReal * aReal + aImag * aImag;
    if (aMag2 < 1e-12f) return kMinDb;
    const float mag2 = bMag2 / aMag2;
    return 10.0f * std::log10(std::max(mag2, 1e-12f));
}

void ClientDeEssCurveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect();
    p.fillRect(r, kBgColor);

    // Grid — vertical at major freqs, horizontal at 0 / -12 / -24.
    p.save();
    p.setPen(QPen(kGridColor, 1.0));
    for (float db : { 0.0f, -12.0f, -24.0f }) {
        const float y = dbToY(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.setPen(QPen(kGridMajorColor, 1.0));
    QFont font = p.font();
    font.setPixelSize(8);
    p.setFont(font);

    // Build the QStaticText cache once — font is fixed at 8 px regardless
    // of compact mode, so this set never needs invalidation after the
    // first paint.  Labels follow kFreqMajor: "100", "500", "1k", etc.
    if (m_labelsDirty) {
        m_axisLabels.clear();
        m_axisLabels.reserve(static_cast<int>(std::size(kFreqMajor)));
        for (float f : kFreqMajor) {
            const QString lbl = (f >= 1000.0f)
                ? QString::number(int(f / 1000.0f)) + "k"
                : QString::number(int(f));
            QStaticText st(lbl);
            st.setPerformanceHint(QStaticText::AggressiveCaching);
            m_axisLabels.append(std::move(st));
        }
        m_labelsDirty = false;
    }

    // drawStaticText anchors at top-left, drawText at the baseline —
    // subtract ascent so the visual position matches the prior render.
    const qreal ascent = p.fontMetrics().ascent();

    for (size_t i = 0; i < std::size(kFreqMajor); ++i) {
        const float f = kFreqMajor[i];
        const float x = hzToX(f);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        if (!m_compact) {
            m_axisLabels[i].prepare(p.transform(), font);
            p.setPen(kAxisLabelColor);
            p.drawStaticText(QPointF(x + 2.0f, r.bottom() - 2.0f - ascent),
                             m_axisLabels[i]);
            p.setPen(QPen(kGridMajorColor, 1.0));
        }
    }
    p.restore();

    // Bandpass magnitude curve.
    if (m_deEss) {
        QPainterPath path;
        constexpr int steps = 121;
        for (int i = 0; i < steps; ++i) {
            const float t   = static_cast<float>(i) / (steps - 1);
            // Log sweep across kMinHz..kMaxHz.
            const float lhz = std::log10(kMinHz)
                + t * (std::log10(kMaxHz) - std::log10(kMinHz));
            const float hz  = std::pow(10.0f, lhz);
            const float db  = std::clamp(bandpassMagDb(hz), kMinDb, kMaxDb);
            const QPointF pt(hzToX(hz), dbToY(db));
            if (i == 0) path.moveTo(pt);
            else        path.lineTo(pt);
        }
        QPen curvePen(kCurveColor, m_compact ? 1.5 : 2.0);
        curvePen.setJoinStyle(Qt::RoundJoin);
        curvePen.setCapStyle(Qt::RoundCap);
        p.setPen(curvePen);
        p.drawPath(path);

        // Live ball at (centreFreq, sidechain level) — tracks energy
        // flowing through the bandpass.  Mapped onto the magnitude
        // curve at the current centre so it reads as "this is what
        // the detector sees."
        const float scDb = std::clamp(m_lastScDb, kMinDb, kMaxDb);
        const float x = hzToX(m_deEss->frequencyHz());
        const float y = dbToY(scDb);
        const float glow = m_compact ? 6.0f : 9.0f;
        QRadialGradient g(QPointF(x, y), glow);
        QColor gc = kBallGlowColor; gc.setAlpha(200);
        g.setColorAt(0.0, gc);
        gc.setAlpha(0);
        g.setColorAt(1.0, gc);
        p.setBrush(g);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(x, y), glow, glow);
        p.setBrush(kBallCoreColor);
        p.drawEllipse(QPointF(x, y), m_compact ? 2.0 : 3.0,
                                      m_compact ? 2.0 : 3.0);

        // Threshold horizontal line — dim cyan.
        QColor tc = kThreshColor; tc.setAlpha(150);
        p.setPen(QPen(tc, 1.2, Qt::DashLine));
        const float yT = dbToY(std::clamp(m_deEss->thresholdDb(),
                                          kMinDb, kMaxDb));
        p.drawLine(QPointF(r.left(), yT), QPointF(r.right(), yT));
    }
}

} // namespace MasterSDR
