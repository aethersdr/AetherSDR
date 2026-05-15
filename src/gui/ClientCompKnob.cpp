#include "ClientCompKnob.h"

#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QPixmap>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr float kDragPxPerFullRange = 200.0f;  // 200 px vertical = 0..1
constexpr float kWheelNormStep      = 0.01f;   // 1% per wheel notch
constexpr float kFineMultiplier     = 0.25f;   // Shift-drag scales ×0.25
constexpr float kArcStartDeg        = 225.0f;  // 7:30 clockwise to
constexpr float kArcSpanDeg         = -270.0f; // 4:30  = 270° sweep

const QColor kRingBg     ("#1a2a3a");
const QColor kRingArc    ("#4db8d4");
const QColor kPointer    ("#e8e8e8");
const QColor kLabelColor ("#b0c4d6");
const QColor kValueColor ("#e8e8e8");

} // namespace

ClientCompKnob::ClientCompKnob(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(58, 64);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void ClientCompKnob::setLabel(const QString& text)
{
    m_label = text;
    update();
}

void ClientCompKnob::setRange(float minPhysical, float maxPhysical)
{
    m_minPhys = minPhysical;
    m_maxPhys = maxPhysical;
    setValue(m_physical);
}

void ClientCompKnob::setDefault(float physical)
{
    m_defaultPhys = physical;
}

void ClientCompKnob::setValueFromNorm(ValueMap fromNorm)
{
    m_fromNorm = std::move(fromNorm);
    setValue(m_physical);  // re-evaluate with new mapping
}

void ClientCompKnob::setNormFromValue(ValueMap toNorm)
{
    m_toNorm = std::move(toNorm);
    setValue(m_physical);
}

void ClientCompKnob::setLabelFormat(LabelFormat fmt)
{
    m_fmt = std::move(fmt);
    update();
}

void ClientCompKnob::setCenterLabelMode(bool on)
{
    if (m_centerLabel == on) return;
    m_centerLabel = on;
    update();
}

void ClientCompKnob::setValue(float physical)
{
    m_physical = std::clamp(physical, m_minPhys, m_maxPhys);
    if (m_toNorm) {
        m_norm = std::clamp(m_toNorm(m_physical), 0.0f, 1.0f);
    } else {
        const float span = std::max(1e-9f, m_maxPhys - m_minPhys);
        m_norm = std::clamp((m_physical - m_minPhys) / span, 0.0f, 1.0f);
    }
    update();
}

void ClientCompKnob::applyNorm(float norm)
{
    m_norm = std::clamp(norm, 0.0f, 1.0f);
    if (m_fromNorm) {
        m_physical = std::clamp(m_fromNorm(m_norm), m_minPhys, m_maxPhys);
    } else {
        m_physical = m_minPhys + m_norm * (m_maxPhys - m_minPhys);
    }
    emit valueChanged(m_physical);
    update();
}

QString ClientCompKnob::formatValue() const
{
    if (m_fmt) return m_fmt(m_physical);
    return QString::number(m_physical, 'f', 2);
}

void ClientCompKnob::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int h = height();
    // Center-label mode reclaims the 12-px label row above the ring.
    // The ring uses almost all remaining height — the 270° arc sweep
    // (225° → -45°) leaves a 90° gap at the bottom (4:30 → 7:30),
    // which is where the value readout lives.  Reserving a separate
    // "row" below the ring creates a visible gap; overlapping the
    // empty arc sector keeps the readout snug against the knob.
    const int topRow    = m_centerLabel ?  0 : 12;
    const int bottomRow = 4;
    const int diameter  = std::min(w - 4, h - topRow - bottomRow);
    const QRectF ring((w - diameter) * 0.5f,
                      static_cast<float>(topRow),
                      diameter, diameter);

    // Label — above the ring in classic mode, centred inside the
    // ring in center-label mode.  Centred text is rendered AFTER
    // the arc + pointer so it reads on top cleanly.
    QFont labelFont = p.font();
    labelFont.setPixelSize(9);
    labelFont.setBold(true);
    if (!m_centerLabel) {
        // Shrink-to-fit: longer labels ("Threshold", "Release") on
        // narrow knobs would otherwise truncate with "..." — scale
        // the font down in 1-px steps until the rendered width fits
        // the widget, floor at 7 px.
        int pixelSize = 9;
        QFontMetrics fm(labelFont);
        const float maxWidth = std::max(8.0f, static_cast<float>(w) - 2.0f);
        while (fm.horizontalAdvance(m_label) > maxWidth && pixelSize > 7) {
            --pixelSize;
            labelFont.setPixelSize(pixelSize);
            fm = QFontMetrics(labelFont);
        }
        p.setFont(labelFont);
        p.setPen(kLabelColor);
        p.drawText(QRectF(0, 0, w, 12), Qt::AlignCenter, m_label);
    }

    // Background ring.
    const qreal thick = std::max(2.0, diameter * 0.10);
    QPen bgPen(kRingBg, thick);
    bgPen.setCapStyle(Qt::FlatCap);
    p.setPen(bgPen);
    p.drawArc(ring.adjusted(thick * 0.5, thick * 0.5,
                            -thick * 0.5, -thick * 0.5),
              static_cast<int>(kArcStartDeg * 16.0f),
              static_cast<int>(kArcSpanDeg * 16.0f));

    // Value arc.
    QPen arcPen(kRingArc, thick);
    arcPen.setCapStyle(Qt::FlatCap);
    p.setPen(arcPen);
    p.drawArc(ring.adjusted(thick * 0.5, thick * 0.5,
                            -thick * 0.5, -thick * 0.5),
              static_cast<int>(kArcStartDeg * 16.0f),
              static_cast<int>((kArcSpanDeg * m_norm) * 16.0f));

    // Pointer tick line from the inner edge of the arc to the inner
    // tick radius — visible indicator of exact knob position.
    const float angle = (kArcStartDeg + kArcSpanDeg * m_norm) * (M_PI / 180.0);
    const QPointF c = ring.center();
    const float rOut = diameter * 0.5f - thick * 0.5f;
    const float rIn  = diameter * 0.5f - thick * 1.6f;
    const QPointF pOut(c.x() + rOut * std::cos(angle),
                       c.y() - rOut * std::sin(angle));
    const QPointF pIn (c.x() + rIn  * std::cos(angle),
                       c.y() - rIn  * std::sin(angle));
    QPen pointerPen(kPointer, thick * 0.6);
    pointerPen.setCapStyle(Qt::RoundCap);
    p.setPen(pointerPen);
    p.drawLine(pIn, pOut);

    // Center-label mode: draw the parameter name centred inside the
    // ring.  The cap is sized so a 6-char label like "Thresh" /
    // "Release" fits inside the ring WITHOUT shrinking — every label
    // then renders at the same size, whether it's "Hold" or "Return".
    // Without the cap the shrink-to-fit fires only on long labels and
    // short labels bloat, visually inconsistent across the row.
    if (m_centerLabel && !m_label.isEmpty()) {
        QFont centerFont = p.font();
        centerFont.setBold(true);
        int pixelSize = std::max(8, diameter / 6);
        centerFont.setPixelSize(pixelSize);

        // Max text width is the knob interior, less a ring-thickness
        // pad on each side so text never kisses the arc.
        const float maxTextWidth = std::max(
            8.0f,
            static_cast<float>(diameter) - 4.0f * static_cast<float>(thick));
        QFontMetrics fm(centerFont);
        while (fm.horizontalAdvance(m_label) > maxTextWidth
               && pixelSize > 7) {
            --pixelSize;
            centerFont.setPixelSize(pixelSize);
            fm = QFontMetrics(centerFont);
        }

        p.setFont(centerFont);
        p.setPen(kLabelColor);
        p.drawText(ring, Qt::AlignCenter, m_label);
    }

    // Value text — positioned inside the empty bottom sector of the
    // 270° arc (between the 4:30 and 7:30 sweep endpoints).  The
    // sector starts at y = ring_center + arc_radius * sin(45°),
    // which works out to ~82 % of the diameter from the ring top.
    QFont valFont = p.font();
    valFont.setPixelSize(11);
    valFont.setBold(false);
    p.setFont(valFont);
    p.setPen(kValueColor);
    const int valueY = topRow + static_cast<int>(diameter * 0.82f);
    const int valueH = std::max(13, h - valueY);
    p.drawText(QRectF(0, valueY, w, valueH),
               Qt::AlignHCenter | Qt::AlignTop,
               formatValue());
}

void ClientCompKnob::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_dragStartY = ev->position().y();
    m_dragStartNorm = m_norm;
    setCursor(Qt::SizeVerCursor);
    ev->accept();
}

void ClientCompKnob::mouseMoveEvent(QMouseEvent* ev)
{
    if (!m_dragging) return;
    const float dy = static_cast<float>(m_dragStartY - ev->position().y());
    const float scale = (ev->modifiers() & Qt::ShiftModifier)
                          ? kFineMultiplier : 1.0f;
    applyNorm(m_dragStartNorm + scale * dy / kDragPxPerFullRange);
    ev->accept();
}

void ClientCompKnob::mouseReleaseEvent(QMouseEvent* ev)
{
    if (!m_dragging) return;
    m_dragging = false;
    setCursor(Qt::ArrowCursor);
    ev->accept();
}

void ClientCompKnob::mouseDoubleClickEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    setValue(m_defaultPhys);
    emit valueChanged(m_physical);
    ev->accept();
}

void ClientCompKnob::wheelEvent(QWheelEvent* ev)
{
    const int ticks = ev->angleDelta().y() / 120;
    const float scale = (ev->modifiers() & Qt::ShiftModifier)
                          ? kFineMultiplier : 1.0f;
    applyNorm(m_norm + ticks * kWheelNormStep * scale);
    ev->accept();
}

void ClientCompKnob::contextMenuEvent(QContextMenuEvent* ev)
{
    // Right-click → numeric entry.  Drag / wheel / double-click stay on
    // the left button so existing muscle memory is untouched; this just
    // adds a precise-value path on a button the knob ignored before.
    const QString param = m_label.isEmpty() ? tr("Value") : m_label;
    QMenu menu(this);
    QAction* enterAct = menu.addAction(tr("Enter %1…").arg(param));
    QAction* resetAct = menu.addAction(tr("Reset to default"));
    QAction* chosen = menu.exec(ev->globalPos());
    if (chosen == enterAct) {
        bool ok = false;
        const double val = QInputDialog::getDouble(
            this,
            tr("Enter %1").arg(param),
            tr("%1 (%2 to %3):")
                .arg(param)
                .arg(static_cast<double>(m_minPhys))
                .arg(static_cast<double>(m_maxPhys)),
            static_cast<double>(m_physical),
            static_cast<double>(m_minPhys),
            static_cast<double>(m_maxPhys),
            3, &ok);
        if (ok) {
            setValue(static_cast<float>(val));  // clamps to [min, max]
            emit valueChanged(m_physical);
        }
    } else if (chosen == resetAct) {
        setValue(m_defaultPhys);
        emit valueChanged(m_physical);
    }
    ev->accept();
}

} // namespace AetherSDR
