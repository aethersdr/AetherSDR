#include "RangeSlider.h"

#include <QMouseEvent>
#include <QPainter>
#include <algorithm>

RangeSlider::RangeSlider(int min, int max, int low, int high,
                         const QString& label, const QString& unit,
                         QWidget* parent)
    : QWidget(parent)
    , m_min(min), m_max(max)
    , m_low(std::clamp(low,  min, max))
    , m_high(std::clamp(high, min, max))
    , m_label(label)
    , m_unit(unit)
    , m_leftLabelW(label.isEmpty() ? kRightLabelW : 55)
{
    setMouseTracking(false);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void RangeSlider::setLow(int v)
{
    v = std::clamp(v, m_min, m_high);
    if (v == m_low) return;
    m_low = v;
    update();
    emit rangeChanged(m_low, m_high);
}

void RangeSlider::setHigh(int v)
{
    v = std::clamp(v, m_low, m_max);
    if (v == m_high) return;
    m_high = v;
    update();
    emit rangeChanged(m_low, m_high);
}

void RangeSlider::setRange(int min, int max)
{
    m_min  = min;
    m_max  = max;
    m_low  = std::clamp(m_low,  min, max);
    m_high = std::clamp(m_high, min, max);
    update();
}

// ── geometry helpers ────────────────────────────────────────────────────────

QRect RangeSlider::grooveRect() const
{
    int gy = (height() - kGrooveH) / 2;
    return QRect(m_leftLabelW, gy, width() - m_leftLabelW - kRightLabelW, kGrooveH);
}

int RangeSlider::valueToX(int val) const
{
    const QRect g = grooveRect();
    if (m_max == m_min) return g.left();
    return g.left() + (val - m_min) * g.width() / (m_max - m_min);
}

int RangeSlider::xToValue(int x) const
{
    const QRect g = grooveRect();
    if (g.width() <= 0) return m_min;
    return m_min + std::clamp(x - g.left(), 0, g.width())
                   * (m_max - m_min) / g.width();
}

QRect RangeSlider::handleRect(int val) const
{
    int cx = valueToX(val);
    int hy = (height() - kHandleH) / 2;
    return QRect(cx - kHandleW / 2, hy, kHandleW, kHandleH);
}

// ── painting ────────────────────────────────────────────────────────────────

void RangeSlider::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRect g = grooveRect();
    const int   xLo = valueToX(m_low);
    const int   xHi = valueToX(m_high);

    // Groove background
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x33, 0x33, 0x33));
    p.drawRoundedRect(g, 2, 2);

    // Highlighted range
    QRect fill(xLo, g.top(), xHi - xLo, g.height());
    p.setBrush(QColor(0x19, 0x76, 0xd2));   // Material blue 700
    p.drawRect(fill);

    // Handles
    auto drawHandle = [&](int val) {
        QRect h = handleRect(val);
        p.setBrush(QColor(0xcc, 0xcc, 0xcc));
        p.setPen(QPen(QColor(0x19, 0x76, 0xd2), 1));
        p.drawRoundedRect(h, 2, 2);
    };
    drawHandle(m_low);
    drawHandle(m_high);

    // Labels
    QFont f = font();
    f.setPixelSize(9);
    p.setFont(f);

    // Left: dim label prefix, bright value
    const QString loVal = QString::number(m_low) + m_unit;
    if (!m_label.isEmpty()) {
        // Draw the prefix in a dimmer color, value in normal color
        const QString prefix = m_label + " ";
        QFontMetrics fm(f);
        const int prefixW = fm.horizontalAdvance(prefix);
        const int totalW  = prefixW + fm.horizontalAdvance(loVal);
        const int startX  = m_leftLabelW - 2 - totalW;

        p.setPen(QColor(0x66, 0x66, 0x66));
        p.drawText(startX, 0, prefixW, height(),
                   Qt::AlignLeft | Qt::AlignVCenter, prefix);
        p.setPen(QColor(0xaa, 0xaa, 0xaa));
        p.drawText(startX + prefixW, 0, fm.horizontalAdvance(loVal) + 2, height(),
                   Qt::AlignLeft | Qt::AlignVCenter, loVal);
    } else {
        p.setPen(QColor(0xaa, 0xaa, 0xaa));
        p.drawText(QRect(0, 0, m_leftLabelW - 2, height()),
                   Qt::AlignRight | Qt::AlignVCenter, loVal);
    }

    // Right: value only
    const QString hiStr = QString::number(m_high) + m_unit;
    p.setPen(QColor(0xaa, 0xaa, 0xaa));
    p.drawText(QRect(width() - kRightLabelW + 2, 0, kRightLabelW - 2, height()),
               Qt::AlignLeft | Qt::AlignVCenter, hiStr);
}

// ── mouse ────────────────────────────────────────────────────────────────────

void RangeSlider::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;

    const int px = e->pos().x();
    const bool nearLow  = std::abs(px - valueToX(m_low))  <= kHandleW;
    const bool nearHigh = std::abs(px - valueToX(m_high)) <= kHandleW;

    if (nearLow && nearHigh) {
        m_dragging = (px <= valueToX(m_low)) ? Handle::Low : Handle::High;
    } else if (nearLow) {
        m_dragging = Handle::Low;
    } else if (nearHigh) {
        m_dragging = Handle::High;
    } else {
        int v = xToValue(px);
        m_dragging = (std::abs(v - m_low) <= std::abs(v - m_high))
                     ? Handle::Low : Handle::High;
    }
    mouseMoveEvent(e);
}

void RangeSlider::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging == Handle::None) return;
    const int v = xToValue(e->pos().x());
    if (m_dragging == Handle::Low)
        m_low  = std::clamp(v, m_min, m_high);
    else
        m_high = std::clamp(v, m_low, m_max);
    update();
    emit rangeChanged(m_low, m_high);
}

void RangeSlider::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = Handle::None;
}
