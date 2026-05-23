#include "ClientCompMeter.h"

#include <QPainter>
#include <QPaintEvent>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr float kLevelMinDb = -60.0f;
constexpr float kLevelMaxDb =   0.0f;
constexpr float kGrMaxMag   =  20.0f;    // GR range 0..-20 dB
constexpr int   kPeakHoldMs =  700;
constexpr float kPeakDecayDbPer100Ms = 1.0f;

const QColor kBarBg    ("#0a1420");
const QColor kLevelLo  ("#56c48b");
const QColor kLevelMid ("#e8d65a");
const QColor kLevelHi  ("#e85a5a");
const QColor kGrColor  ("#e8a540");
const QColor kLabelColor("#b0c4d6");
const QColor kPeakLine ("#ffffff");
const QColor kCeilingLine("#f2c14e");     // bright amber — matches LIMIT button
const QColor kCeilingZone("#3a1810");     // dim red tint for the "no-go" zone
const QColor kLimGrTick ("#4db8d4");      // cyan, distinct from the white peak line

} // namespace

ClientCompMeter::ClientCompMeter(QWidget* parent) : QWidget(parent)
{
    setMinimumWidth(18);
    setMinimumHeight(80);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_peakHoldTimer.start();

    m_animTimer.setTimerType(Qt::PreciseTimer);
    m_animTimer.setInterval(kMeterSmootherIntervalMs);
    connect(&m_animTimer, &QTimer::timeout, this, [this]() {
        if (!m_smooth.tick(m_animElapsed.restart()))
            m_animTimer.stop();
        update();
    });
}

void ClientCompMeter::setMode(Mode m)
{
    if (m == m_mode) return;
    m_mode = m;
    m_currentDb = -120.0f;
    m_peakDb    = -120.0f;
    m_smooth.setTarget(0.0f);
    m_smooth.snapToTarget();
    update();
}

void ClientCompMeter::setLabel(const QString& label)
{
    m_label = label;
    update();
}

void ClientCompMeter::setLimiterCeilingDb(float db)
{
    if (std::fabs(db - m_ceilingDb) < 0.01f) return;
    m_ceilingDb = db;
    update();
}

void ClientCompMeter::setLimiterGrDb(float db)
{
    if (std::fabs(db - m_limGrDb) < 0.01f) return;
    m_limGrDb = db;
    update();
}

void ClientCompMeter::recomputeTarget()
{
    float target = 0.0f;
    if (m_mode == Mode::Level) {
        target = std::clamp(
            (m_currentDb - kLevelMinDb) / (kLevelMaxDb - kLevelMinDb),
            0.0f, 1.0f);
    } else {
        const float mag = std::clamp(-m_currentDb, 0.0f, kGrMaxMag);
        target = mag / kGrMaxMag;
    }
    m_smooth.setTarget(target);
    if (!m_smooth.needsAnimation()) {
        if (m_animTimer.isActive()) m_animTimer.stop();
    } else if (!m_animTimer.isActive()) {
        m_animElapsed.restart();
        m_animTimer.start();
    }
}

void ClientCompMeter::setValueDb(float db)
{
    // Peak-hold tracks the loudest recent reading (or largest GR) and
    // decays after a 700 ms hold.  Jumps instantly to any new extreme
    // so transients don't get hidden.  Bar fill is smoothed separately
    // via MeterSmoother.
    if (m_mode == Mode::Level) {
        m_currentDb = db;
        if (db > m_peakDb) {
            m_peakDb = db;
            m_peakHoldTimer.restart();
        } else if (m_peakHoldTimer.elapsed() > kPeakHoldMs) {
            m_peakDb -= kPeakDecayDbPer100Ms / 10.0f;
            m_peakDb = std::max(m_peakDb, m_currentDb);
        }
    } else {
        m_currentDb = db;
        if (db < m_peakDb) {
            m_peakDb = db;
            m_peakHoldTimer.restart();
        } else if (m_peakHoldTimer.elapsed() > kPeakHoldMs) {
            m_peakDb += kPeakDecayDbPer100Ms / 10.0f;
            m_peakDb = std::min(m_peakDb, m_currentDb);
        }
    }
    recomputeTarget();
}

void ClientCompMeter::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int h = height();
    const int labelH = m_label.isEmpty() ? 0 : 12;
    const QRectF bar(2.0, labelH + 2.0, w - 4.0, h - labelH - 4.0);

    if (!m_label.isEmpty()) {
        QFont f = p.font();
        f.setPixelSize(9);
        f.setBold(true);
        p.setFont(f);
        p.setPen(kLabelColor);
        p.drawText(QRectF(0, 0, w, labelH), Qt::AlignCenter, m_label);
    }

    p.fillRect(bar, kBarBg);

    if (m_mode == Mode::Level) {
        const float fillH = m_smooth.value() * bar.height();
        QRectF fill(bar.left(), bar.bottom() - fillH, bar.width(), fillH);

        QLinearGradient g(0, bar.bottom(), 0, bar.top());
        g.setColorAt(0.0, kLevelLo);
        g.setColorAt(0.7, kLevelMid);
        g.setColorAt(1.0, kLevelHi);
        p.fillRect(fill, g);

        // Limiter overlay — draw ceiling zone + line if a ceiling has
        // been set.  m_ceilingDb > 0 is our sentinel for "no overlay"
        // (ceilings are always ≤ 0 dBFS in practice).
        if (m_ceilingDb <= kLevelMaxDb + 0.0001f) {
            const float tc = std::clamp(
                (m_ceilingDb - kLevelMinDb) / (kLevelMaxDb - kLevelMinDb),
                0.0f, 1.0f);
            const float cy = bar.bottom() - tc * bar.height();

            // Red-zone shading above the ceiling — "do not enter."
            const QRectF zone(bar.left(), bar.top(),
                              bar.width(), cy - bar.top());
            if (zone.height() > 0) {
                QColor zoneColor = kCeilingZone;
                zoneColor.setAlpha(140);
                p.fillRect(zone, zoneColor);
            }

            // Ceiling line — bright amber, slightly thicker than the
            // peak line so it reads as a structural limit, not a meter
            // value.
            p.setPen(QPen(kCeilingLine, 1.5));
            p.drawLine(QPointF(bar.left() - 2.0, cy),
                       QPointF(bar.right() + 2.0, cy));

            // Limiter GR tick — cyan stub hanging from the ceiling
            // line into the bar whenever the limiter is clamping.
            // Length ∝ |m_limGrDb|, capped at 12 dB so big spikes
            // don't blow past the bar.
            if (m_limGrDb < -0.05f) {
                const float grSpanDb = std::min(-m_limGrDb, 12.0f);
                const float tickH =
                    (grSpanDb / (kLevelMaxDb - kLevelMinDb)) * bar.height();
                p.setPen(QPen(kLimGrTick, 2.0));
                p.drawLine(QPointF(bar.center().x(), cy),
                           QPointF(bar.center().x(), cy + tickH));
            }
        }

        if (m_peakDb > kLevelMinDb) {
            const float tp = std::clamp(
                (m_peakDb - kLevelMinDb) / (kLevelMaxDb - kLevelMinDb),
                0.0f, 1.0f);
            const float y = bar.bottom() - tp * bar.height();
            p.setPen(QPen(kPeakLine, 1.0));
            p.drawLine(QPointF(bar.left(), y), QPointF(bar.right(), y));
        }
    } else {
        const float fillH = m_smooth.value() * bar.height();
        QRectF fill(bar.left(), bar.top(), bar.width(), fillH);
        p.fillRect(fill, kGrColor);

        const float peakMag = std::clamp(-m_peakDb, 0.0f, kGrMaxMag);
        if (peakMag > 0.0f) {
            const float y = bar.top() + (peakMag / kGrMaxMag) * bar.height();
            p.setPen(QPen(kPeakLine, 1.0));
            p.drawLine(QPointF(bar.left(), y), QPointF(bar.right(), y));
        }
    }
}

} // namespace MasterSDR
