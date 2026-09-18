#include "NrGainStrip.h"

#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>

#include <algorithm>

namespace AetherSDR {

namespace {
// The AetherModem status-strip palette: near-black scope bed, phosphor green
// for signal, a dim green for the suppressed floor.
const QColor kBed(0x03, 0x08, 0x0e);
const QColor kBorder(0x23, 0x32, 0x46);
const QColor kSignal(0x63, 0xd4, 0x7a);
const QColor kFloor(0x1f, 0x6e, 0x46);
const QColor kIdle(0x2a, 0x3a, 0x4a);
} // namespace

NrGainStrip::NrGainStrip(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(QStringLiteral("Noise reduction gain"));
    setToolTip(QStringLiteral(
        "How much the active noise-reduction method is passing through.\n"
        "High = speech is coming through, low = the gap between syllables is\n"
        "being suppressed. Flat baseline means no method is running."));
    setAccessibleDescription(toolTip());
    m_sweep.setInterval(kFrameMs);
    connect(&m_sweep, &QTimer::timeout, this, [this] { advance(); });
    resizeBuffers(220);
}

void NrGainStrip::setGain(float gain, bool active)
{
    m_pending = std::clamp(gain, 0.0f, 1.0f);
    m_active = active;
}

void NrGainStrip::reset()
{
    std::fill(m_values.begin(), m_values.end(), 0.0f);
    std::fill(m_signal.begin(), m_signal.end(), quint8(0));
    m_cursor = 0;
    update();
}

void NrGainStrip::resizeBuffers(int width)
{
    const int n = std::max(8, width);
    if (m_values.size() == n) {
        return;
    }
    m_values.assign(n, 0.0f);
    m_signal.assign(n, quint8(0));
    m_cursor = 0;
}

void NrGainStrip::advance()
{
    if (m_values.isEmpty()) {
        return;
    }
    m_values[m_cursor] = m_active ? m_pending : 0.0f;
    m_signal[m_cursor] =
        (m_active && m_pending >= kSignalGain) ? quint8(1) : quint8(0);
    m_cursor = (m_cursor + 1) % m_values.size();
    update();
}

void NrGainStrip::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    resizeBuffers(width());
}

void NrGainStrip::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_sweep.start();
}

void NrGainStrip::hideEvent(QHideEvent* event)
{
    // Nothing to draw while hidden, and the applet spends most of its life
    // collapsed — the same reason PacketActivityWidget stops its own sweep.
    m_sweep.stop();
    QWidget::hideEvent(event);
}

void NrGainStrip::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bed = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(kBorder, 1));
    painter.setBrush(kBed);
    painter.drawRoundedRect(bed, 4, 4);

    const int n = m_values.size();
    if (n < 8) {
        return;
    }

    const float base = height() - 4.0f;
    const float usable = height() - 8.0f;

    if (!m_active) {
        painter.setPen(QPen(kIdle, 1));
        painter.drawLine(QPointF(2.0, base), QPointF(width() - 2.0, base));
        return;
    }

    // Draw oldest-to-newest so the trail behind the cursor reads as history.
    // Signal and floor are separate passes rather than a per-segment pen swap:
    // two polylines cost less than one pen change per pixel column.
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantSignal = (pass == 1);
        painter.setPen(QPen(wantSignal ? kSignal : kFloor, wantSignal ? 1.6 : 1.0));
        QPainterPath path;
        bool open = false;
        for (int x = 0; x < n && x < width(); ++x) {
            const int idx = (m_cursor + x) % n;
            if ((m_signal[idx] != 0) != wantSignal) {
                open = false;
                continue;
            }
            const QPointF pt(x, base - m_values[idx] * usable);
            if (!open) {
                path.moveTo(pt);
                open = true;
            } else {
                path.lineTo(pt);
            }
        }
        painter.drawPath(path);
    }

    // The sweep cursor sits at the newest sample, at the right-hand end of the
    // trail rather than wherever the ring buffer happens to be indexed.
    const qreal cursorX = std::min<qreal>(width() - 2.0, n - 1);
    painter.setPen(QPen(QColor(0x80, 0xed, 0x91, 0xb0), 1));
    painter.drawLine(QPointF(cursorX, 2.0), QPointF(cursorX, height() - 2.0));
}

} // namespace AetherSDR
