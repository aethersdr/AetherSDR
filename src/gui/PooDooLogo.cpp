#include "PooDooLogo.h"
#include "core/ClientPudu.h"

#include <QFont>
#include <QPainter>
#include <QPaintEvent>
#include <QRadialGradient>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

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
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &PooDooLogo::tick);
}

void PooDooLogo::setPudu(ClientPudu* p)
{
    m_pudu = p;
    if (m_pudu) m_timer->start();
    else        m_timer->stop();
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
    f.setPixelSize(static_cast<int>(r.height() * 0.55f));
    f.setWeight(QFont::Black);
    p.setFont(f);

    const QString wordmark = m_wordmark.isEmpty()
        ? QString::fromUtf8("PooDoo\xe2\x84\xa2")
        : m_wordmark;
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

} // namespace MasterSDR
