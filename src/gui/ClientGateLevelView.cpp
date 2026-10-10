#include "ClientGateLevelView.h"
#include "PanelTick.h"
#include "core/ClientGate.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QTimer>
#include <algorithm>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

constexpr int kHistoryCount = 240;      // ~4 s at kPanelTickMs
constexpr int kPollMs       = kPanelTickMs;

inline QColor kBgColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.background.0"); }
inline QColor kFrameColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.background.1"); }
inline QColor kGridColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.background.1"); }
inline QColor kAxisLabel(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.text.label"); }
inline QColor kInputColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.text.primary"); }  // white input outline
inline QColor kAudibleColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.accent.warning"); }  // amber — audio that passes through
inline QColor kReduceColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.background.1"); }  // dark gray reduction overlay
inline QColor kThreshColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.accent.dim"); }  // cyan threshold lines
inline QColor kPeakColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.accent.warning"); }  // amber peak bar

// dB ticks shown on the left gutter.
const float kTicks[] = { 6.0f, 0.0f, -6.0f, -12.0f, -18.0f,
                         -24.0f, -36.0f, -50.0f, -70.0f };

constexpr int   kGutterPx  = 28;   // left label gutter
constexpr int   kPeakBarPx = 8;    // right-edge peak strip

} // namespace

ClientGateLevelView::ClientGateLevelView(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(240, 160);
    setAttribute(Qt::WA_OpaquePaintEvent, false);

    m_history.assign(kHistoryCount, Sample{kBottomDb, 0.0f});

    m_timer = new QTimer(this);
    m_timer->setInterval(kPollMs);
    connect(m_timer, &QTimer::timeout, this, &ClientGateLevelView::tick);
}

void ClientGateLevelView::setGate(ClientGate* gate)
{
    m_gate = gate;
    // Only while on screen: see PanelTick.h. Binding a model to a widget
    // that is not visible used to start a poll nothing would ever stop,
    // because a never-shown widget gets no hideEvent.
    if (m_gate && isVisible()) m_timer->start();
    else                         m_timer->stop();
    update();
}

void ClientGateLevelView::tick()
{
    if (!m_gate) return;
    Sample s;
    s.inputDb = std::clamp(m_gate->inputPeakDb(), kBottomDb, kTopDb);
    s.grDb    = std::clamp(m_gate->gainReductionDb(), -80.0f, 0.0f);
    m_history[m_writeIdx] = s;
    m_writeIdx = (m_writeIdx + 1) % kHistoryCount;
    update();
}

float ClientGateLevelView::dbToY(float db) const
{
    const float t = (db - kBottomDb) / (kTopDb - kBottomDb);
    const float top    = 4.0f;
    const float bottom = static_cast<float>(height()) - 4.0f;
    return bottom - t * (bottom - top);
}

void ClientGateLevelView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF full = rect();
    p.fillRect(full, kBgColor(this));

    // Plot region: inset the left gutter for dB labels and the right
    // strip for the peak bar.
    const QRectF plot(full.left() + kGutterPx, full.top() + 4,
                      full.width() - kGutterPx - kPeakBarPx - 4,
                      full.height() - 8);

    // Frame the plot.
    p.setPen(QPen(kFrameColor(this), 1.0));
    p.drawRect(plot);

    // Horizontal grid + labels.
    QFont f = p.font();
    f.setPixelSize(9);
    p.setFont(f);
    for (float db : kTicks) {
        const float y = dbToY(db);
        p.setPen(QPen(kGridColor(this), 1.0));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(kAxisLabel(this));
        QString s = (db > 0.0f ? "+" : "") + QString::number(static_cast<int>(db));
        p.drawText(QPointF(full.left() + 2.0f, y + 3.0f), s);
    }

    // Newest sample at the right edge. Draw order: (1) audible band, bottom to
    // input+grDb, amber; (2) gated band, input+grDb to input, dark gray;
    // (3) input envelope outline in white on top.
    p.save();
    p.setClipRect(plot);

    const float colW = plot.width() / float(kHistoryCount);

    // ── Audible band (amber) ───────────────────────────────────
    QPainterPath audiblePath;
    for (int i = 0; i < kHistoryCount; ++i) {
        const int idx = (m_writeIdx + i) % kHistoryCount;
        const Sample& s = m_history[idx];
        const float x = plot.right() - (kHistoryCount - 1 - i) * colW;
        // When gate is fully open, grDb = 0 → yGr = yIn; the amber
        // band touches the input outline.  When the gate closes,
        // grDb is negative and yGr sits lower → amber band shrinks,
        // revealing the gray gated band above it.
        const float yGr = dbToY(s.inputDb + s.grDb);
        if (i == 0) audiblePath.moveTo(x, yGr);
        else        audiblePath.lineTo(x, yGr);
    }
    audiblePath.lineTo(plot.right(), plot.bottom());
    audiblePath.lineTo(plot.left(),  plot.bottom());
    audiblePath.closeSubpath();

    QColor audibleFill = kAudibleColor(this); audibleFill.setAlpha(90);
    p.setBrush(audibleFill);
    p.setPen(Qt::NoPen);
    p.drawPath(audiblePath);

    // ── Gated band (dark gray) ──────────────────────────────────
    QPainterPath gatedPath;
    for (int i = 0; i < kHistoryCount; ++i) {
        const int idx = (m_writeIdx + i) % kHistoryCount;
        const Sample& s = m_history[idx];
        const float x = plot.right() - (kHistoryCount - 1 - i) * colW;
        const float yIn = dbToY(s.inputDb);
        if (i == 0) gatedPath.moveTo(x, yIn);
        else        gatedPath.lineTo(x, yIn);
    }
    for (int i = kHistoryCount - 1; i >= 0; --i) {
        const int idx = (m_writeIdx + i) % kHistoryCount;
        const Sample& s = m_history[idx];
        const float x = plot.right() - (kHistoryCount - 1 - i) * colW;
        const float yGr = dbToY(s.inputDb + s.grDb);
        gatedPath.lineTo(x, yGr);
    }
    gatedPath.closeSubpath();

    p.setBrush(kReduceColor(this));
    p.setPen(Qt::NoPen);
    p.drawPath(gatedPath);

    // ── Input top outline ───────────────────────────────────────
    QPainterPath inputOutline;
    for (int i = 0; i < kHistoryCount; ++i) {
        const int idx = (m_writeIdx + i) % kHistoryCount;
        const Sample& s = m_history[idx];
        const float x = plot.right() - (kHistoryCount - 1 - i) * colW;
        const float yIn = dbToY(s.inputDb);
        if (i == 0) inputOutline.moveTo(x, yIn);
        else        inputOutline.lineTo(x, yIn);
    }
    p.setPen(QPen(kInputColor(this), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(inputOutline);

    p.restore();

    // Threshold lines.
    if (m_gate) {
        const float T  = m_gate->thresholdDb();
        const float Tc = T - m_gate->returnDb();
        QPen pen(kThreshColor(this), 1.5);
        p.setPen(pen);
        const float yT  = dbToY(std::clamp(T,  kBottomDb, kTopDb));
        p.drawLine(QPointF(plot.left(), yT), QPointF(plot.right(), yT));
        QColor returnCol = kThreshColor(this); returnCol.setAlpha(180);
        p.setPen(QPen(returnCol, 1.2));
        const float yTc = dbToY(std::clamp(Tc, kBottomDb, kTopDb));
        p.drawLine(QPointF(plot.left(), yTc), QPointF(plot.right(), yTc));
    }

    // Peak bar on the right edge — current input level in amber.
    {
        const QRectF bar(full.right() - kPeakBarPx - 2, plot.top(),
                         kPeakBarPx, plot.height());
        p.fillRect(bar, kBgColor(this));
        p.setPen(QPen(kFrameColor(this), 1.0));
        p.drawRect(bar);

        if (m_gate) {
            const float inDb = std::clamp(
                m_gate->inputPeakDb(), kBottomDb, kTopDb);
            const float yIn = dbToY(inDb);
            const QRectF fill(bar.left() + 1, yIn,
                              bar.width() - 2, bar.bottom() - yIn - 1);
            p.fillRect(fill, kPeakColor(this));
        }
    }
}


void ClientGateLevelView::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (m_gate && m_timer) m_timer->start();
}

void ClientGateLevelView::hideEvent(QHideEvent* ev)
{
    if (m_timer) m_timer->stop();
    QWidget::hideEvent(ev);
}

} // namespace AetherSDR
