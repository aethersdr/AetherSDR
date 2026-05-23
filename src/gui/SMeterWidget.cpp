#include "SMeterWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QtMath>
#include <QFontMetrics>

namespace MasterSDR {

// --- Construction ------------------------------------------------------------

SMeterWidget::SMeterWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(minimumSizeHint());
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_needleFraction = dbmToFraction(m_levelDbm);
    m_targetNeedleFraction = m_needleFraction;
    m_peakHoldDecayStartDbm = m_peakHoldDbm;

    m_needleAnimation.setTimerType(Qt::PreciseTimer);
    m_needleAnimation.setInterval(kNeedleAnimationIntervalMs);
    connect(&m_needleAnimation, &QTimer::timeout, this, &SMeterWidget::animateNeedle);

    // Peak hold decay: drops 0.5 dB every 50 ms after a new peak
    m_peakDecay.setInterval(50);
    connect(&m_peakDecay, &QTimer::timeout, this, [this]() {
        m_peakDbm -= 0.5f;
        if (m_peakDbm < m_levelDbm) {
            m_peakDbm = m_levelDbm;
            m_peakDecay.stop();
        }
        updateNeedleTarget();
        update();
    });

    // Hard reset peak hold every 10 seconds
    m_peakReset.setInterval(10000);
    m_peakReset.start();
    connect(&m_peakReset, &QTimer::timeout, this, [this]() {
        m_peakDbm = m_levelDbm;
        updateNeedleTarget();
        update();
    });
}

// --- Public interface --------------------------------------------------------

void SMeterWidget::setLevel(float dbm)
{
    m_levelDbm = dbm;

    // Peak hold (existing needle/triangle behavior)
    if (dbm > m_peakDbm) {
        m_peakDbm = dbm;
        m_peakDecay.start();
    }

    // Configurable peak hold line
    if (m_peakHoldEnabled) {
        if (dbm > m_peakHoldDbm) {
            m_peakHoldDbm = dbm;
            m_peakHoldDecayStartDbm = dbm;
            m_peakHoldTimer.start();
            m_peakHoldTimerRunning = true;
        }
        updatePeakHoldValue();
    }

    updateNeedleTarget();

    if (!m_transmitting) {
        update();
    }
}

void SMeterWidget::setTxMeters(float fwdPower, float swr)
{
    m_txPower = fwdPower;
    m_txSwr = swr;

    updateNeedleTarget();

    // Repaint whenever TX power data arrives — either because moxChanged set
    // m_transmitting, or because RF power is flowing regardless (e.g. VOX,
    // hardware-keyed CW, or interlock race where setTransmitting arrives late).
    if (m_transmitting || m_txPower > 0.5f) {
        update();
    }
}

void SMeterWidget::setMicMeters(float micLevel, float compLevel, float micPeak, float compPeak)
{
    Q_UNUSED(micLevel);
    Q_UNUSED(compLevel);
    m_micLevel = micPeak;
    // compPeak is raw dBFS from COMPPEAK. Silence gate at -30.
    const float comp = (compPeak > -30.0f) ? qBound(-25.0f, compPeak, 0.0f) : 0.0f;
    m_compLevel = comp;

    updateNeedleTarget();

    if (m_transmitting && (m_txMode == TxMode::Level || m_txMode == TxMode::Compression)) {
        update();
    }
}

void SMeterWidget::setTransmitting(bool tx)
{
    m_transmitting = tx;
    if (!tx) {
        // Clear TX values immediately on un-key so the RX reading becomes the
        // animation target as soon as transmit ends.
        m_txPower = 0.0f;
        m_txSwr   = 1.0f;
    }
    updateNeedleTarget();
    update();
}

void SMeterWidget::setTxMode(const QString& mode)
{
    if (mode == "Power")            m_txMode = TxMode::Power;
    else if (mode == "SWR")         m_txMode = TxMode::SWR;
    else if (mode == "Level")       m_txMode = TxMode::Level;
    else if (mode == "Compression") m_txMode = TxMode::Compression;
    updateNeedleTarget();
    update();
}

void SMeterWidget::setRxMode(const QString& mode)
{
    if (mode == "S-Meter") {
        m_rxMode = RxMode::SMeter;
        m_source = "S-Meter";
    } else {
        m_rxMode = RxMode::SMeterPeak;
        m_source = "S-Meter Peak";
    }
    updateNeedleTarget();
    update();
}

void SMeterWidget::updateNeedleTarget()
{
    updatePeakHoldValue();

    if (m_transmitting) {
        m_targetNeedleFraction = txValueToFraction(currentTxValue());
    } else if (m_rxMode == RxMode::SMeterPeak) {
        m_targetNeedleFraction = dbmToFraction(m_peakDbm);
    } else {
        m_targetNeedleFraction = dbmToFraction(m_levelDbm);
    }

    const bool needleAtTarget = qAbs(m_targetNeedleFraction - m_needleFraction) <= kNeedleSnapEpsilon;
    if (needleAtTarget) {
        m_needleFraction = m_targetNeedleFraction;
    }

    const bool peakHoldAnimating = m_peakHoldEnabled
        && m_peakHoldTimerRunning
        && m_peakHoldTimer.elapsed() > m_peakHoldTimeMs
        && m_peakHoldDbm > m_levelDbm + 0.01f;

    if (needleAtTarget && !peakHoldAnimating) {
        if (m_needleAnimation.isActive()) {
            m_needleAnimation.stop();
        }
        return;
    }

    if (!m_needleAnimation.isActive()) {
        m_needleElapsed.restart();
        m_needleAnimation.start();
    }
}

void SMeterWidget::animateNeedle()
{
    const qint64 elapsedMs = m_needleElapsed.restart();
    if (elapsedMs <= 0) {
        return;
    }

    updatePeakHoldValue();

    const float delta = m_targetNeedleFraction - m_needleFraction;
    const float elapsedSeconds = static_cast<float>(elapsedMs) / 1000.0f;
    const float timeConstant = (delta >= 0.0f) ? kNeedleAttackTimeSeconds
                                                : kNeedleReleaseTimeSeconds;
    const float alpha = 1.0f - std::exp(-elapsedSeconds / timeConstant);
    const bool needleAtTarget = qAbs(delta) <= kNeedleSnapEpsilon;
    if (needleAtTarget) {
        m_needleFraction = m_targetNeedleFraction;
    } else {
        m_needleFraction += delta * alpha;
    }

    const bool peakHoldAnimating = m_peakHoldEnabled
        && m_peakHoldTimerRunning
        && m_peakHoldTimer.elapsed() > m_peakHoldTimeMs
        && m_peakHoldDbm > m_levelDbm + 0.01f;

    if (needleAtTarget && !peakHoldAnimating) {
        m_needleAnimation.stop();
    }

    update();
}

void SMeterWidget::updatePeakHoldValue()
{
    if (!m_peakHoldEnabled || !m_peakHoldTimerRunning) {
        return;
    }

    const qint64 elapsedMs = m_peakHoldTimer.elapsed();
    if (elapsedMs <= m_peakHoldTimeMs) {
        return;
    }

    const float decayElapsedSeconds =
        static_cast<float>(elapsedMs - m_peakHoldTimeMs) / 1000.0f;
    m_peakHoldDbm = m_peakHoldDecayStartDbm - (m_peakDecayDbPerSec * decayElapsedSeconds);
    if (m_peakHoldDbm <= m_levelDbm) {
        m_peakHoldDbm = m_levelDbm;
    }
}

QString SMeterWidget::sUnitsText() const
{
    if (m_levelDbm <= S0_DBM) return "S0";
    if (m_levelDbm <= S9_DBM) {
        const int s = qRound((m_levelDbm - S0_DBM) / DB_PER_S);
        return QString("S%1").arg(qBound(0, s, 9));
    }
    const int over = qRound(m_levelDbm - S9_DBM);
    return QString("S9+%1").arg(over);
}

// --- Mapping -----------------------------------------------------------------

float SMeterWidget::dbmToFraction(float dbm) const
{
    // S0 to S9 occupies the left 60% of the arc
    // S9 to S9+60 occupies the right 40%
    const float clamped = qBound(S0_DBM, dbm, MAX_DBM);

    if (clamped <= S9_DBM) {
        // Linear within S0..S9 -> 0.0..0.6
        return 0.6f * (clamped - S0_DBM) / (S9_DBM - S0_DBM);
    }
    // Linear within S9..S9+60 -> 0.6..1.0
    return 0.6f + 0.4f * (clamped - S9_DBM) / (MAX_DBM - S9_DBM);
}

float SMeterWidget::txValueToFraction(float value) const
{
    switch (m_txMode) {
    case TxMode::Power:
        return qBound(0.0f, value / m_powerScaleMax, 1.0f);
    case TxMode::SWR:
        // 1.0-3.0
        return qBound(0.0f, (value - 1.0f) / 2.0f, 1.0f);
    case TxMode::Level:
        // -40 to +5
        return qBound(0.0f, (value + 40.0f) / 45.0f, 1.0f);
    case TxMode::Compression:
        // Gain reduction: 0 = none, -25 = heavy compression
        return qBound(0.0f, (value + 25.0f) / 25.0f, 1.0f);
    }
    return 0.0f;
}

float SMeterWidget::currentTxValue() const
{
    switch (m_txMode) {
    case TxMode::Power:       return m_txPower;
    case TxMode::SWR:         return m_txSwr;
    case TxMode::Level:       return m_micLevel;
    case TxMode::Compression: return m_compLevel;
    }
    return 0.0f;
}

// --- Paint -------------------------------------------------------------------

void SMeterWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int w = width();
    const int h = height();

    // Background
    p.fillRect(rect(), QColor(0x0f, 0x0f, 0x1a));

    // -- Arc geometry ---------------------------------------------------------
    // Large radius with center far below widget -> shallow ~70 deg arc segment
    const float cx = w * 0.5f;
    const float radius = w * 0.85f;
    const float cy = h + radius - h * 0.65f;  // arc center well below widget
    const float needleCy = h + 6.0f;          // needle origin just below widget bottom

    // Convert arc degrees to radians
    const float arcStartRad = qDegreesToRadians(ARC_START_DEG);
    const float arcEndRad   = qDegreesToRadians(ARC_END_DEG);
    const float arcSpanRad  = arcEndRad - arcStartRad;

    // fraction 0.0 -> left end (ARC_END_DEG), fraction 1.0 -> right end (ARC_START_DEG)
    auto fractionToAngle = [&](float frac) -> float {
        return arcEndRad - frac * arcSpanRad;  // radians
    };

    // -- Draw colored outer arc (RX scale) ------------------------------------
    // White from S0 to S9, red from S9+
    {
        const QRectF outerArc(cx - radius, cy - radius, radius * 2, radius * 2);
        const float s9Deg = qRadiansToDegrees(fractionToAngle(0.6f));

        QPen whitePen(QColor(0xc8, 0xd8, 0xe8), 3);
        p.setPen(whitePen);
        p.drawArc(outerArc,
                  static_cast<int>(s9Deg * 16),
                  static_cast<int>((ARC_END_DEG - s9Deg) * 16));

        QPen redPen(QColor(0xff, 0x44, 0x44), 3);
        p.setPen(redPen);
        p.drawArc(outerArc,
                  static_cast<int>(ARC_START_DEG * 16),
                  static_cast<int>((s9Deg - ARC_START_DEG) * 16));
    }

    // -- Draw colored inner arc (TX scale) -- 6px gap -------------------------
    const float arcGap = 6.0f;
    const QColor blueColor(0x00, 0x80, 0xd0);
    const QColor redColor(0xff, 0x44, 0x44);
    {
        const float innerR = radius - arcGap;
        const QRectF innerArc(cx - innerR, cy - innerR, innerR * 2, innerR * 2);

        // Determine where the arc color splits (fraction where red begins)
        float redFrac = -1.0f;  // -1 = no red zone
        switch (m_txMode) {
        case TxMode::Power:       redFrac = m_powerRedStart / m_powerScaleMax; break;
        case TxMode::SWR:         redFrac = (2.5f - 1.0f) / 2.0f; break; // 0.75
        case TxMode::Level:       redFrac = (0.0f + 40.0f) / 45.0f; break; // ~0.89
        case TxMode::Compression: redFrac = -1.0f; break; // all blue
        }

        if (redFrac < 0.0f) {
            // Entire arc is blue
            p.setPen(QPen(blueColor, 3));
            p.drawArc(innerArc,
                      static_cast<int>(ARC_START_DEG * 16),
                      static_cast<int>((ARC_END_DEG - ARC_START_DEG) * 16));
        } else {
            const float splitDeg = qRadiansToDegrees(fractionToAngle(redFrac));
            p.setPen(QPen(blueColor, 3));
            p.drawArc(innerArc,
                      static_cast<int>(splitDeg * 16),
                      static_cast<int>((ARC_END_DEG - splitDeg) * 16));
            p.setPen(QPen(redColor, 3));
            p.drawArc(innerArc,
                      static_cast<int>(ARC_START_DEG * 16),
                      static_cast<int>((splitDeg - ARC_START_DEG) * 16));
        }
    }

    // -- Tick drawing helpers -------------------------------------------------
    QFont tickFont = font();
    tickFont.setPixelSize(qMax(10, h / 10));
    tickFont.setBold(true);
    p.setFont(tickFont);
    const QFontMetrics tfm(tickFont);

    // Direction from needle origin through arc point, normalized
    auto needleDir = [&](float angle) -> std::pair<float, float> {
        const float arcX = cx + radius * std::cos(angle);
        const float arcY = cy - radius * std::sin(angle);
        const float dx = arcX - cx;
        const float dy = arcY - needleCy;
        const float len = std::sqrt(dx * dx + dy * dy);
        return {dx / len, dy / len};
    };

    // Outside tick (RX): extends outward from the arc, label above
    auto drawOutsideTick = [&](float frac, const QString& label, const QColor& color,
                               bool showLabel) {
        const float angle = fractionToAngle(frac);
        const float arcX = cx + radius * std::cos(angle);
        const float arcY = cy - radius * std::sin(angle);
        auto [ux, uy] = needleDir(angle);

        const QPointF inner(arcX + 2 * ux, arcY + 2 * uy);
        const QPointF outer(arcX + 14 * ux, arcY + 14 * uy);

        p.setPen(QPen(color, 1.5));
        p.drawLine(inner, outer);

        if (showLabel) {
            const QPointF labelPt(arcX + 26 * ux, arcY + 26 * uy);
            const int tw = tfm.horizontalAdvance(label);
            p.setPen(color);
            p.drawText(QPointF(labelPt.x() - tw / 2.0,
                               labelPt.y() + tfm.ascent() / 2.0), label);
        }
    };

    // Inside tick (TX): extends inward from the inner colored arc
    const float innerArcR = radius - arcGap;
    auto drawInsideTick = [&](float frac, const QString& label,
                              const QColor& tickColor, const QColor& labelColor,
                              bool showLabel) {
        const float angle = fractionToAngle(frac);
        // Start from the inner colored arc, not the outer arc
        const float iArcX = cx + innerArcR * std::cos(angle);
        const float iArcY = cy - innerArcR * std::sin(angle);
        auto [ux, uy] = needleDir(angle);

        const QPointF outer(iArcX - 2 * ux, iArcY - 2 * uy);
        const QPointF inner(iArcX - 14 * ux, iArcY - 14 * uy);

        p.setPen(QPen(tickColor, 1.5));
        p.drawLine(inner, outer);

        if (showLabel) {
            const QPointF labelPt(iArcX - 26 * ux, iArcY - 26 * uy);
            const int tw = tfm.horizontalAdvance(label);
            p.setPen(labelColor);
            p.drawText(QPointF(labelPt.x() - tw / 2.0,
                               labelPt.y() + tfm.ascent() / 2.0), label);
        }
    };

    const QColor whiteColor(0xc8, 0xd8, 0xe8);

    // -- Outside ticks (RX): S-meter scale -- odd S-units only ----------------
    for (int s = 1; s <= 9; s += 2) {
        const float dbm = S0_DBM + s * DB_PER_S;
        drawOutsideTick(dbmToFraction(dbm), QString::number(s), whiteColor, true);
    }
    for (int over : {20, 40}) {
        const float dbm = S9_DBM + over;
        drawOutsideTick(dbmToFraction(dbm), QString("+%1").arg(over), redColor, true);
    }

    // -- Inside ticks (TX): scale depends on TX mode --------------------------
    switch (m_txMode) {
    case TxMode::Power: {
        // Dynamic scale based on m_powerScaleMax
        int maxW = static_cast<int>(m_powerScaleMax);
        int redW = static_cast<int>(m_powerRedStart);
        int tickStep, labelStep;
        if (maxW >= 2000) {         // PGXL: ticks every 100W, labels every 500W
            tickStep = 100; labelStep = 500;
        } else if (maxW >= 600) {   // Aurora: ticks every 50W, labels every 100W
            tickStep = 50; labelStep = 100;
        } else {                    // Barefoot: ticks every 10W, labels every 40W
            tickStep = 10; labelStep = 40;
        }
        for (int w = 0; w <= maxW; w += tickStep) {
            const float frac = static_cast<float>(w) / m_powerScaleMax;
            const QColor& tc = (w >= redW) ? redColor : blueColor;
            const QColor& lc = (w >= redW) ? redColor : whiteColor;
            bool isLabeled = (w % labelStep == 0) || w == maxW || w == redW;
            QString label = (w >= 1000) ? QString("%1k").arg(w / 1000.0f, 0, 'f', (w % 1000) ? 1 : 0)
                                        : QString::number(w);
            drawInsideTick(frac, label, tc, lc, isLabeled);
        }
        break;
    }
    case TxMode::SWR: {
        // 1.0-3.0, ticks at 1, 1.5, 2, 2.5, 3.  Red starting at 2.5.
        for (float s : {1.0f, 1.5f, 2.0f, 2.5f, 3.0f}) {
            const float frac = (s - 1.0f) / 2.0f;
            const bool red = (s >= 2.5f);
            const QColor& tc = red ? redColor : blueColor;
            const QColor& lc = red ? redColor : whiteColor;
            QString label = (s == static_cast<int>(s))
                ? QString::number(static_cast<int>(s))
                : QString::number(s, 'f', 1);
            drawInsideTick(frac, label, tc, lc, true);
        }
        break;
    }
    case TxMode::Level: {
        // -40 to +5, ticks at -40, -30, -20, -10, 0.  Red at 0.
        for (int db : {-40, -30, -20, -10, 0}) {
            const float frac = (db + 40.0f) / 45.0f;
            const bool red = (db >= 0);
            const QColor& tc = red ? redColor : blueColor;
            const QColor& lc = red ? redColor : whiteColor;
            drawInsideTick(frac, QString::number(db), tc, lc, true);
        }
        break;
    }
    case TxMode::Compression: {
        // -25 to 0, ticks at -25, -20, -15, -10, -5, 0.  All default color.
        for (int db : {-25, -20, -15, -10, -5, 0}) {
            const float frac = (db + 25.0f) / 25.0f;
            drawInsideTick(frac, QString::number(db), blueColor, whiteColor, true);
        }
        break;
    }
    }

    // -- Draw needle ----------------------------------------------------------
    // Needle originates from needleCy (just below widget) rather than the
    // arc center, so the pivot is barely out of frame.
    // When transmitting, needle tracks the selected TX meter instead of RX.
    {
        const float angle = fractionToAngle(m_needleFraction);

        // Needle extends to the end of the outer (RX) ticks: radius + 14
        const float tipR = radius + 14;
        const float tipX = cx + tipR * std::cos(angle);
        const float tipY = cy - tipR * std::sin(angle);

        // Needle shadow
        p.setPen(QPen(QColor(0, 0, 0, 80), 3));
        p.drawLine(QPointF(cx + 1, needleCy + 1), QPointF(tipX + 1, tipY + 1));

        // Needle
        p.setPen(QPen(QColor(0xff, 0xff, 0xff), 2));
        p.drawLine(QPointF(cx, needleCy), QPointF(tipX, tipY));
    }

    // Draw peak marker (small triangle) — only in RX S-Meter Peak mode
    if (!m_transmitting && m_rxMode == RxMode::SMeterPeak
        && m_peakDbm > m_levelDbm + 1.0f) {
        const float frac = dbmToFraction(m_peakDbm);
        const float angle = fractionToAngle(frac);
        const float markerR = radius - 2;

        const float cosA = std::cos(angle);
        const float sinA = std::sin(angle);

        const QPointF tip(cx + markerR * cosA, cy - markerR * sinA);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xff, 0xaa, 0x00));
        const float perpCos = -sinA;
        const float perpSin = cosA;
        const float sz = 3.0f;
        QPainterPath tri;
        tri.moveTo(tip);
        tri.lineTo(tip.x() - 6 * cosA + sz * perpCos,
                   tip.y() + 6 * sinA + sz * perpSin);
        tri.lineTo(tip.x() - 6 * cosA - sz * perpCos,
                   tip.y() + 6 * sinA - sz * perpSin);
        tri.closeSubpath();
        p.drawPath(tri);
    }

    // -- Draw peak hold line (configurable overlay, independent of RX mode) ---
    if (m_peakHoldEnabled && !m_transmitting
        && m_peakHoldDbm > S0_DBM + 1.0f) {
        float frac = dbmToFraction(m_peakHoldDbm);
        if (m_peakHoldDbm <= m_levelDbm + 0.01f) {
            frac = m_needleFraction;
        } else {
            frac = qMax(frac, m_needleFraction);
        }
        const float angle = fractionToAngle(frac);

        const float cosA = std::cos(angle);
        const float sinA = std::sin(angle);
        const QPointF inner(cx + (radius - 4) * cosA,
                            cy - (radius - 4) * sinA);
        const QPointF outer(cx + (radius + 10) * cosA,
                            cy - (radius + 10) * sinA);

        p.setPen(QPen(QColor(0xff, 0x44, 0x44, 0xcc), 2));
        p.drawLine(inner, outer);
    }

    // -- Text readout -- all top-aligned on the same baseline -----------------
    QFont srcFont = font();
    srcFont.setPixelSize(qMax(9, h / 14));
    const QFontMetrics sfm(srcFont);
    const int topY = sfm.height() + 2;

    QFont valFont = font();
    valFont.setPixelSize(qMax(13, h / 8));
    valFont.setBold(true);
    const QFontMetrics vfm(valFont);

    if (m_transmitting) {
        // TX mode: show TX source label (center), mode name (left), value (right)
        static const char* txLabels[] = {"Power", "SWR", "Level", "Compression"};
        const QString srcLabel = txLabels[static_cast<int>(m_txMode)];
        p.setFont(srcFont);
        p.setPen(QColor(0x80, 0x90, 0xa0));
        p.drawText((w - sfm.horizontalAdvance(srcLabel)) / 2, topY, srcLabel);

        p.setFont(valFont);
        // Left: mode name in cyan
        p.setPen(QColor(0x00, 0xb4, 0xd8));
        p.drawText(6, topY, "TX");

        // Right: formatted value
        QString valText;
        switch (m_txMode) {
        case TxMode::Power:       valText = QString("%1 W").arg(m_txPower, 0, 'f', 0); break;
        case TxMode::SWR:         valText = QString("%1").arg(m_txSwr, 0, 'f', 1); break;
        case TxMode::Level:       valText = QString("%1 dB").arg(m_micLevel, 0, 'f', 0); break;
        case TxMode::Compression: valText = QString("%1 dB").arg(m_compLevel, 0, 'f', 0); break;
        }
        p.setPen(QColor(0xc8, 0xd8, 0xe8));
        p.drawText(w - vfm.horizontalAdvance(valText) - 6, topY, valText);
    } else {
        // RX mode: show source label (center), S-units (left), dBm (right)
        p.setFont(srcFont);
        p.setPen(QColor(0x80, 0x90, 0xa0));
        p.drawText((w - sfm.horizontalAdvance(m_source)) / 2, topY, m_source);

        const float displayDbm = (m_rxMode == RxMode::SMeterPeak) ? m_peakDbm : m_levelDbm;

        p.setFont(valFont);
        p.setPen(QColor(0x00, 0xb4, 0xd8));
        // Show S-units based on the displayed value
        QString sText;
        if (displayDbm <= S0_DBM) sText = "S0";
        else if (displayDbm <= S9_DBM) {
            sText = QString("S%1").arg(qBound(0, qRound((displayDbm - S0_DBM) / DB_PER_S), 9));
        } else {
            sText = QString("S9+%1").arg(qRound(displayDbm - S9_DBM));
        }
        p.drawText(6, topY, sText);

        const QString dbmText = QString("%1 dBm").arg(displayDbm, 0, 'f', 0);
        p.setPen(QColor(0xc8, 0xd8, 0xe8));
        p.drawText(w - vfm.horizontalAdvance(dbmText) - 6, topY, dbmText);
    }
}

void SMeterWidget::setPowerScale(int maxWatts, bool hasAmplifier)
{
    if (hasAmplifier) {
        m_powerScaleMax = 2000.0f;
        m_powerRedStart = 1500.0f;
    } else if (maxWatts > 100) {
        m_powerScaleMax = 600.0f;
        m_powerRedStart = 500.0f;
    } else {
        m_powerScaleMax = 120.0f;
        m_powerRedStart = 100.0f;
    }
    updateNeedleTarget();
    update();
}

// --- Peak hold configuration -------------------------------------------------

void SMeterWidget::setPeakHoldEnabled(bool enabled)
{
    m_peakHoldEnabled = enabled;
    m_peakHoldDbm = m_levelDbm;
    m_peakHoldDecayStartDbm = m_levelDbm;
    m_peakHoldTimerRunning = false;
    updateNeedleTarget();
    update();
}

void SMeterWidget::setPeakHoldTimeMs(int ms)
{
    m_peakHoldTimeMs = qBound(100, ms, 2000);
}

void SMeterWidget::setPeakDecayRate(DecayRate rate)
{
    switch (rate) {
    case DecayRate::Fast:   m_peakDecayDbPerSec = 20.0f; break;
    case DecayRate::Medium: m_peakDecayDbPerSec = 10.0f; break;
    case DecayRate::Slow:   m_peakDecayDbPerSec = 5.0f;  break;
    }
}

void SMeterWidget::setPeakDecayRate(const QString& rate)
{
    if (rate == "Fast")        setPeakDecayRate(DecayRate::Fast);
    else if (rate == "Slow")   setPeakDecayRate(DecayRate::Slow);
    else                       setPeakDecayRate(DecayRate::Medium);
}

void SMeterWidget::resetPeak()
{
    m_peakHoldDbm = m_levelDbm;
    m_peakHoldDecayStartDbm = m_levelDbm;
    m_peakHoldTimerRunning = false;
    updateNeedleTarget();
    update();
}

} // namespace MasterSDR
