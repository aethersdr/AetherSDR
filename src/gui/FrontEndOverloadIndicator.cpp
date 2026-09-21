#include "gui/FrontEndOverloadIndicator.h"

#include <QAccessible>
#include <QAccessibleEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>

using AetherSDR::FrontEndLevel;
using AetherSDR::gui::LampColour;

namespace {
constexpr int kLampDiameter = 10;
constexpr int kLampGap = 6;

QColor colourFor(LampColour c)
{
    switch (c) {
    case LampColour::Dark:  return QColor(0x3a, 0x3a, 0x3a);
    case LampColour::Green: return QColor(0x2e, 0xa0, 0x43);
    case LampColour::Amber: return QColor(0xd2, 0x8a, 0x00);
    case LampColour::Red:   return QColor(0xc4, 0x2b, 0x1c);
    }
    return QColor(0x3a, 0x3a, 0x3a);
}
}  // namespace

FrontEndOverloadIndicator::FrontEndOverloadIndicator(QWidget* parent)
    : QWidget(parent)
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addSpacing(kLampDiameter + kLampGap);   // the lamp is painted, not a child
    m_text = new QLabel(this);
    row->addWidget(m_text);
    row->addStretch(1);

    // FOCUSABLE ON PURPOSE. This is a status readout, so it is not in the tab
    // order by default -- but docs/a11y.md asks that a blind operator be able to
    // go and READ a state rather than only be told about it when it changes, and
    // an unfocusable widget with an accessible name is not reachable that way.
    setFocusPolicy(Qt::TabFocus);
    setAccessibleName(tr("Front end"));

    m_latchTimer.setSingleShot(true);
    m_latchTimer.setInterval(kRedLatchMs);
    connect(&m_latchTimer, &QTimer::timeout, this, [this] { update(); });

    refresh();
}

AetherSDR::gui::LampColour FrontEndOverloadIndicator::shownLamp() const
{
    const LampColour now = AetherSDR::gui::lampFor(m_state.level);
    if (now == LampColour::Red) {
        return LampColour::Red;
    }
    if (m_redSince.isValid() && m_redSince.elapsed() < kRedLatchMs) {
        return LampColour::Red;
    }
    return now;
}

void FrontEndOverloadIndicator::setState(const AetherSDR::FrontEndOverload& state)
{
    if (state == m_state) {
        return;
    }
    const FrontEndLevel before = m_state.level;
    m_state = state;

    if (AetherSDR::gui::lampFor(m_state.level) == LampColour::Red) {
        m_redSince.start();
        m_latchTimer.start();
    }

    refresh();
    announceIfWorthIt(before);
}

void FrontEndOverloadIndicator::refresh()
{
    m_text->setText(AetherSDR::gui::shortText(m_state));
    const QString spoken = AetherSDR::gui::accessibleText(m_state);
    setAccessibleDescription(spoken);
    m_text->setAccessibleDescription(spoken);
    setToolTip(spoken);
    update();
}

void FrontEndOverloadIndicator::announceIfWorthIt(FrontEndLevel before)
{
    if (!AetherSDR::gui::shouldAnnounce(before, m_state.level)) {
        return;
    }
    if (!QAccessible::isActive()) {
        return;
    }
    // AN ANNOUNCEMENT, NOT A VALUE CHANGE, and not gated on focus.
    //
    // Every other announcing widget in this tree (RangeSlider, HGauge) speaks
    // only while focused, because they are reporting what the operator is
    // DOING. This one reports what the RADIO is doing, to an operator who is
    // most likely looking at the panadapter -- and a blind operator has no
    // panadapter to look at, so a focus-gated announcement would mean the one
    // user who most needs to hear about a clipping front end is the one who
    // never does. That is the gap #4896 exists to close.
    //
    // Politeness, not assertiveness: it must not interrupt an announcement the
    // operator asked for. shouldAnnounce() is what keeps this from becoming
    // chatter -- only starting, stopping and hitting the floor speak at all.
    QAccessibleAnnouncementEvent ev(this, AetherSDR::gui::accessibleText(m_state));
    ev.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&ev);
}

QSize FrontEndOverloadIndicator::sizeHint() const
{
    const QSize t = m_text ? m_text->sizeHint() : QSize(0, kLampDiameter);
    return QSize(kLampDiameter + kLampGap + t.width(),
                 std::max(t.height(), kLampDiameter));
}

void FrontEndOverloadIndicator::paintEvent(QPaintEvent* e)
{
    QWidget::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int y = (height() - kLampDiameter) / 2;
    const QColor c = colourFor(shownLamp());
    p.setPen(QPen(c.darker(160), 1.0));
    p.setBrush(c);
    p.drawEllipse(QRectF(0.5, y + 0.5, kLampDiameter - 1.0, kLampDiameter - 1.0));

    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(palette().highlight().color(), 1.0));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}
