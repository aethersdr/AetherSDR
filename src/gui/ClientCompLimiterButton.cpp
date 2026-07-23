#include "ClientCompLimiterButton.h"

#include <QPainter>
#include <QPaintEvent>
#include <QRadialGradient>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

// Three-state colour palette:
//   - Disarmed (limiter disabled) — dim, grey, clearly "off"
//   - Armed (limiter enabled, not firing) — dark green body, bright green text
//   - Active (firing, held 500 ms) — red body + halo, white text
inline QColor kBgArmed() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kBgDisarmed() { return AetherSDR::ThemeManager::instance().color("color.background.0"); }
inline QColor kBgActive() { return AetherSDR::ThemeManager::instance().color("color.accent.danger"); }
inline QColor kBorderArm() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kBorderOff() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kBorderHit() { return AetherSDR::ThemeManager::instance().color("color.accent.danger"); }
inline QColor kTextArmed() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kTextOff() { return AetherSDR::ThemeManager::instance().color("color.text.label"); }
inline QColor kTextActive() { return AetherSDR::ThemeManager::instance().color("color.text.primary"); }
} // namespace

ClientCompLimiterButton::ClientCompLimiterButton(QWidget* parent)
    : QPushButton(parent)
{
    setCheckable(true);
    setFlat(false);
    setText("LIMIT");
    setFixedHeight(22);
    m_activeHold.start();
}

void ClientCompLimiterButton::setActive(bool active)
{
    if (active) {
        m_activeHold.restart();
        if (!m_active) { m_active = true; update(); }
        return;
    }
    if (m_active && m_activeHold.elapsed() > kHoldMs) {
        m_active = false;
        update();
    }
}

void ClientCompLimiterButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Half-pixel inset so the 1px antialiased border lands on pixel centres
    // (crisp). QRectF::adjusted takes qreal, so 0.5 is honoured — unlike
    // QRect::adjusted, which truncated it to 0 and left the stroke straddling
    // the edge (soft).
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = 3.0;

    // Colour the body by state precedence: active > checked > idle.
    QColor bg;
    QColor border;
    QColor textColor;
    if (m_active) {
        bg = kBgActive();
        border = kBorderHit();
        textColor = kTextActive();
    } else if (isChecked()) {
        bg = kBgArmed();
        border = kBorderArm();
        textColor = kTextArmed();
    } else {
        bg = kBgDisarmed();
        border = kBorderOff();
        textColor = kTextOff();
    }

    if (underMouse() && !m_active) {
        bg = bg.lighter(120);
    }

    p.setBrush(bg);
    p.setPen(QPen(border, 1.0));
    p.drawRoundedRect(r, radius, radius);

    // Active halo — soft red glow around the button when firing.  Small
    // but unmistakable; the strobe + colour change reads like a clipping
    // indicator on hardware gear.
    if (m_active) {
        QRadialGradient g(r.center(), r.width() * 0.6);
        QColor glow = kBgActive();
        glow.setAlpha(90);
        g.setColorAt(0.0, glow);
        glow.setAlpha(0);
        g.setColorAt(1.0, glow);
        p.setBrush(g);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(r.adjusted(-3, -3, 3, 3), radius + 3, radius + 3);
    }

    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(textColor);
    p.drawText(r, Qt::AlignCenter, text());
}

} // namespace AetherSDR
