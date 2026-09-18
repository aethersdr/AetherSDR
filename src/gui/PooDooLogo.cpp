#include "PooDooLogo.h"
#include "PanelTick.h"
#include "core/ClientPudu.h"

#include <QFont>
#include <QFontMetricsF>
#include <algorithm>
#include <QPainter>
#include <QPaintEvent>
#include <QRadialGradient>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr float kSmoothAlpha = 0.25f;
constexpr float kMinDb = -60.0f;   // treat quieter than this as "off"
constexpr float kMaxDb =   0.0f;   // full pulse at 0 dBFS wet RMS

} // namespace

PooDooLogo::PooDooLogo(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(42);
    setAttribute(Qt::WA_OpaquePaintEvent, false);

    m_timer = new QTimer(this);
    m_timer->setInterval(kPanelTickMs);
    connect(m_timer, &QTimer::timeout, this, &PooDooLogo::tick);
}

void PooDooLogo::setPudu(ClientPudu* p)
{
    m_pudu = p;
    // Only while on screen: see PanelTick.h. Binding a model to a widget
    // that is not visible used to start a poll nothing would ever stop,
    // because a never-shown widget gets no hideEvent.
    if (m_pudu && isVisible()) m_timer->start();
    else                         m_timer->stop();
    update();
}

void PooDooLogo::setWordmark(const QString& mark)
{
    if (m_wordmark == mark) return;
    m_wordmark = mark;
    update();
}

void PooDooLogo::tick()
{
    if (!m_pudu) return;
    const float target = m_pudu->wetRmsDb();
    m_smoothedWetDb += kSmoothAlpha * (target - m_smoothedWetDb);
    update();
}

void PooDooLogo::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect();

    // Background — nothing, lets the applet background show through.

    // Pulse intensity 0..1 mapped from smoothedWetDb in [kMinDb, kMaxDb].
    const float pulse = std::clamp(
        (m_smoothedWetDb - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);

    // Colour — amber with alpha scaled by pulse.  Even at rest we
    // want the logo visible (text is always legible), so text alpha
    // has a floor; the GLOW is what pulses dramatically.
    const int textAlpha = 180 + static_cast<int>(pulse * 75.0f);
    QColor textColor(0xf2, 0xc1, 0x4e, std::clamp(textAlpha, 0, 255));

    // Glow — a radial gradient centred on the logo, intensity scales
    // with pulse.  Fully off at rest (alpha 0) → bright amber halo
    // when the exciter is working hard.
    if (pulse > 0.02f) {
        const QPointF centre = r.center();
        const float radius = r.width() * 0.55f;
        QRadialGradient glow(centre, radius);
        QColor glowColor = textColor;
        glowColor.setAlpha(static_cast<int>(pulse * 120.0f));
        glow.setColorAt(0.0, glowColor);
        glowColor.setAlpha(0);
        glow.setColorAt(1.0, glowColor);
        p.setBrush(glow);
        p.setPen(Qt::NoPen);
        p.drawEllipse(centre, radius, radius * 0.6);
    }

    // PooDoo™ wordmark — bold amber text, centred.
    QFont f = p.font();
    f.setFamily("Arial Black");        // bold, closest to a logo face
    f.setWeight(QFont::Black);

    const QString wordmark = m_wordmark.isEmpty()
        ? QString::fromUtf8("PooDoo\xe2\x84\xa2")
        : m_wordmark;

    // Size off the height, then take it back down until it fits the width.
    // The mark is set per instance — "PooDoo™" on some surfaces, "AetherVoice™"
    // on the strip panel — and height alone sized the longer one straight off
    // both edges of the widget, losing its first and last glyph. Advance is
    // near-linear in pixel size, so one proportional correction lands it; the
    // loop is a guard for the rounding, not the method.
    int px = std::max(8, static_cast<int>(r.height() * 0.55f));
    const qreal room = r.width() * 0.92;    // a little air at each end
    f.setPixelSize(px);
    qreal advance = QFontMetricsF(f).horizontalAdvance(wordmark);
    if (advance > room && advance > 0.0) {
        px = std::max(8, static_cast<int>(px * room / advance));
        for (int guard = 0; guard < 8 && px > 8; ++guard) {
            f.setPixelSize(px);
            advance = QFontMetricsF(f).horizontalAdvance(wordmark);
            if (advance <= room) break;
            --px;
        }
    }
    f.setPixelSize(px);
    p.setFont(f);

    p.setPen(textColor);
    p.drawText(r, Qt::AlignCenter, wordmark);

    // Subtle scanline / chrome underline — a horizontal dim line
    // under the text grounds the logo visually.  Alpha also pulses
    // so the whole mark feels tied together.
    QColor underline(0x5a, 0x3a, 0x0e,
                     80 + static_cast<int>(pulse * 120.0f));
    p.setPen(QPen(underline, 1.2));
    const float uy = r.bottom() - 3.0f;
    const float ux1 = r.left() + r.width() * 0.15f;
    const float ux2 = r.right() - r.width() * 0.15f;
    p.drawLine(QPointF(ux1, uy), QPointF(ux2, uy));
}


void PooDooLogo::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (m_pudu && m_timer) m_timer->start();
}

void PooDooLogo::hideEvent(QHideEvent* ev)
{
    if (m_timer) m_timer->stop();
    QWidget::hideEvent(ev);
}

} // namespace AetherSDR
