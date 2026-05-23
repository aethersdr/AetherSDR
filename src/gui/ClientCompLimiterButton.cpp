#include "ClientCompLimiterButton.h"

#include <QPainter>
#include <QPaintEvent>
#include <QRadialGradient>

namespace MasterSDR {

namespace {

// Three-state colour palette:
//   - Disarmed (limiter disabled) — dim, grey, clearly "off"
//   - Armed (limiter enabled, not firing) — dark green body, bright green text
//   - Active (firing, held 500 ms) — red body + halo, white text
const QColor kBgArmed    ("#006040");
const QColor kBgDisarmed ("#0e1b28");
const QColor kBgActive   ("#d03030");
const QColor kBorderArm  ("#00a060");
const QColor kBorderOff  ("#2a3a4a");
const QColor kBorderHit  ("#ff8080");
const QColor kTextArmed  ("#00ff88");
const QColor kTextOff    ("#607888");
const QColor kTextActive ("#ffffff");

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

    const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = 3.0;

    // Colour the body by state precedence: active > checked > idle.
    QColor bg;
    QColor border;
    QColor textColor;
    if (m_active) {
        bg = kBgActive;
        border = kBorderHit;
        textColor = kTextActive;
    } else if (isChecked()) {
        bg = kBgArmed;
        border = kBorderArm;
        textColor = kTextArmed;
    } else {
        bg = kBgDisarmed;
        border = kBorderOff;
        textColor = kTextOff;
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
        QColor glow = kBgActive;
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

} // namespace MasterSDR
