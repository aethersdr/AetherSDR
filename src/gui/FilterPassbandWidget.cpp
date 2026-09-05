#include "FilterPassbandWidget.h"
#include "FilterPassbandMath.h"
#include "MacCursorCompat.h"

#include <QPainterPath>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR {

FilterPassbandWidget::FilterPassbandWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(minimumSizeHint());
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setCursor(macSafeCursorShape(Qt::SizeAllCursor));
}

void FilterPassbandWidget::setFilter(int lo, int hi)
{
    if (lo == m_lo && hi == m_hi) return;
    m_lo = lo;
    m_hi = hi;
    update();
}

void FilterPassbandWidget::setMode(const QString& mode)
{
    if (mode == m_mode) return;
    m_mode = mode;
    update();
}

void FilterPassbandWidget::setWidthRange(int minimumHz, int maximumHz, int stepHz)
{
    const bool validRange = minimumHz > 0 && maximumHz >= minimumHz;
    m_minimumWidthHz = validRange ? minimumHz : 50;
    m_maximumWidthHz = validRange ? maximumHz : 0;
    m_widthStepHz = validRange && stepHz > 0 ? stepHz : 50;
    update();
}

// ─── Paint ──────────────────────────────────────────────────────────────────

void FilterPassbandWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int w = width();
    const int h = height();

    // Background
    p.fillRect(rect(), QColor(0x0a, 0x0a, 0x18));

    // Border
    p.setPen(QColor(0x20, 0x30, 0x40));
    p.drawRect(0, 0, w - 1, h - 1);

    // ── Static trapezoid shape ──────────────────────────────────────────
    // Fixed geometry — shape doesn't change with filter width
    const int margin = 16;
    const int topY = 14;
    const int botY = h - 16;  // room for labels
    const int loX = margin;
    const int hiX = w - margin;
    constexpr int SKIRT = 16;

    // Filter shape: left skirt, flat top, right skirt (no bottom, no fill)
    p.setPen(QPen(QColor(0x00, 0xb4, 0xd8), 1.5));
    p.drawLine(loX, botY, loX + SKIRT, topY);          // left skirt
    p.drawLine(loX + SKIRT, topY, hiX - SKIRT, topY);  // flat top
    p.drawLine(hiX - SKIRT, topY, hiX, botY);           // right skirt

    // Dashed vertical lines at filter edges (8px inside the skirt vertices)
    p.setPen(QPen(QColor(0x00, 0xb4, 0xd8, 120), 1, Qt::DashLine));
    p.drawLine(loX + SKIRT + 8, 2, loX + SKIRT + 8, h - 2);
    p.drawLine(hiX - SKIRT - 8, 2, hiX - SKIRT - 8, h - 2);

    // ── Labels ──────────────────────────────────────────────────────────
    QFont font = p.font();
    font.setPixelSize(10);
    p.setFont(font);
    const QFontMetrics fm(font);

    // Bandwidth label (centered at bottom)
    int bw = std::abs(m_hi - m_lo);
    QString bwText = bw >= 1000 ? QString("%1.%2K").arg(bw / 1000).arg((bw % 1000) / 100)
                                : QString::number(bw);
    p.setPen(QColor(0xc8, 0xd8, 0xe8));
    p.drawText((loX + hiX) / 2 - fm.horizontalAdvance(bwText) / 2, botY + 12, bwText);

    // Passband center offset (distance from carrier to filter center, below top line)
    // Show the actual signed offsets — LSB / CW filters have negative
    // lo/hi values relative to the carrier and the user needs to see
    // the sign to know which side of the tuned frequency they are. (#2259)
    int center = (m_lo + m_hi) / 2;
    QString centerText = std::abs(center) >= 1000
        ? QString("%1%2.%3K").arg(center < 0 ? "-" : "")
                              .arg(std::abs(center) / 1000)
                              .arg((std::abs(center) % 1000) / 100)
        : QString::number(center);
    p.setPen(QColor(0x90, 0xa0, 0xb0));
    p.drawText((loX + hiX) / 2 - fm.horizontalAdvance(centerText) / 2, topY + 12, centerText);

    // Lo label (centered on left slant bottom point)
    QString loText = QString::number(m_lo);
    p.setPen(QColor(0x80, 0x90, 0xa0));
    p.drawText(loX - fm.horizontalAdvance(loText) / 2, botY + 12, loText);

    // Hi label (centered on right slant bottom point)
    QString hiText = QString::number(m_hi);
    p.drawText(hiX - fm.horizontalAdvance(hiText) / 2, botY + 12, hiText);
}

// ─── Mouse interaction ──────────────────────────────────────────────────────

void FilterPassbandWidget::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    m_dragStartPos = ev->pos();
    m_dragStartLo = m_lo;
    m_dragStartHi = m_hi;

    // Three zones: left of lo line = drag low edge, right of hi line = drag high edge, center = shift
    constexpr int margin = 16, SKIRT = 16;
    const int loLineX = margin + SKIRT + 8;
    const int hiLineX = width() - margin - SKIRT - 8;

    if (ev->pos().x() <= loLineX) {
        m_dragMode = DragLo;
        setCursor(Qt::SizeHorCursor);
    } else if (ev->pos().x() >= hiLineX) {
        m_dragMode = DragHi;
        setCursor(Qt::SizeHorCursor);
    } else {
        m_dragMode = DragShift;
        setCursor(macSafeCursorShape(Qt::SizeAllCursor));
    }
}

void FilterPassbandWidget::mouseMoveEvent(QMouseEvent* ev)
{
    // Hover cursor feedback — three zones: left edge, center, right edge
    // Only call setCursor when the shape actually changes to avoid
    // excessive CGImageCreate calls on macOS (EXC_BREAKPOINT in Qt cursor code).
    if (m_dragMode == DragNone) {
        constexpr int margin = 16, SKIRT = 16;
        const int loLineX = margin + SKIRT + 8;
        const int hiLineX = width() - margin - SKIRT - 8;
        const int x = ev->pos().x();
        const Qt::CursorShape wanted = macSafeCursorShape(
            (x <= loLineX || x >= hiLineX)
                ? Qt::SizeHorCursor : Qt::SizeAllCursor);
        if (cursor().shape() != wanted) {
            setCursor(wanted);
        }
        return;
    }

    const int dx = ev->pos().x() - m_dragStartPos.x();
    const int dy = ev->pos().y() - m_dragStartPos.y();

    const int usableW = width() - 32;
    const int usableH = height();
    const bool radioWidthRange = m_maximumWidthHz > 0;
    // The legacy 6 kHz / 4 kHz spans are gesture scales, not filter limits.
    // Empty capabilities (including disconnect) must restore that contract.
    const double hzPerPxH = passbandDragScaleHzPerPixel(
        radioWidthRange ? m_maximumWidthHz : 6000, usableW);
    const double hzPerPxV = passbandDragScaleHzPerPixel(
        radioWidthRange ? m_maximumWidthHz - m_minimumWidthHz : 4000, usableH);
    const auto snapHz = [this, radioWidthRange](int hz) {
        if (!radioWidthRange) {
            return (hz / 50) * 50;
        }
        return static_cast<int>(std::round(
            static_cast<double>(hz) / m_widthStepHz)) * m_widthStepHz;
    };

    int newLo = m_dragStartLo;
    int newHi = m_dragStartHi;

    if (m_dragMode == DragShift) {
        // Horizontal: shift passband, vertical: symmetric width
        int shiftHz = static_cast<int>(dx * hzPerPxH);
        int bwChange = static_cast<int>(-dy * hzPerPxV);
        shiftHz = snapHz(shiftHz);
        bwChange = snapHz(bwChange);
        newLo = m_dragStartLo + shiftHz - bwChange / 2;
        newHi = m_dragStartHi + shiftHz + bwChange / 2;
    } else if (m_dragMode == DragLo) {
        int deltaHz = static_cast<int>(dx * hzPerPxH);
        deltaHz = snapHz(deltaHz);
        newLo = m_dragStartLo + deltaHz;
    } else if (m_dragMode == DragHi) {
        int deltaHz = static_cast<int>(dx * hzPerPxH);
        deltaHz = snapHz(deltaHz);
        newHi = m_dragStartHi + deltaHz;
    }

    // Enforce the connected radio's mode-specific width range while keeping
    // the edge the operator did not touch anchored.
    const PassbandDragEdge draggedEdge = m_dragMode == DragLo
        ? PassbandDragEdge::Low
        : m_dragMode == DragHi ? PassbandDragEdge::High : PassbandDragEdge::Both;
    const PassbandEdgePair constrained = constrainPassbandWidth(
        newLo, newHi, m_minimumWidthHz,
        radioWidthRange ? m_maximumWidthHz : std::numeric_limits<int>::max(),
        draggedEdge);
    newLo = constrained.lowHz;
    newHi = constrained.highHz;

    // Snap to the radio's width grid.
    newLo = snapHz(newLo);
    newHi = snapHz(newHi);

    if (newLo != m_lo || newHi != m_hi) {
        m_lo = newLo;
        m_hi = newHi;
        update();
        emit filterChanged(m_lo, m_hi);
    }
}

void FilterPassbandWidget::mouseReleaseEvent(QMouseEvent*)
{
    m_dragMode = DragNone;
}

} // namespace AetherSDR
