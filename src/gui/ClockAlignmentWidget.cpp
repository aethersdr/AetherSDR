#include "ClockAlignmentWidget.h"

#include "core/ThemeManager.h"

#include <QChar>
#include <QColor>
#include <QFont>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizePolicy>
#include <QString>
#include <QVector>

#include <algorithm>

// AetherClock alignment scope. Renders one ClockAlignmentFrame per visible
// second (oldest -> newest, left -> right; newest hugs the right edge):
//
//   - received AM envelope as the primary trace, with the matched-filter
//     template of the decoded symbol overlaid on the same vertical scale so
//     the eye reads how well the received second matches the ideal;
//   - a per-second tick at the detected AM-drop edge (offset within the 1 s
//     window, NIST time-code second-edge concept);
//   - a classification-confidence lane (bar height = margin);
//   - a decoded-symbol glyph lane ('0' / '1' / 'M' marker / '?' unknown).
//
// GUI only: paints exclusively from the frame payload in paintEvent, no
// decoder / engine / radio access and no timers (repaints ride the 1 Hz
// appendFrame signal). Per-paint cost is O(window x envelope size).

namespace AetherSDR {

namespace {

// Colours resolve through ThemeManager (canonical tokens, RFC #3076) so a
// runtime theme switch repaints correctly — same idiom as WaveformWidget.
// Accessors, never cached across paints. Token mapping documented in
// docs/theming/canonical-tokens.md.
inline QColor cEnvelope() { return AetherSDR::ThemeManager::instance().color("color.text.primary"); }
inline QColor cExpected() { return AetherSDR::theme::withAlpha("color.accent", 200); }
inline QColor cConfSuccess() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor cConfWarning() { return AetherSDR::ThemeManager::instance().color("color.accent.warning"); }
inline QColor cConfInactive() { return AetherSDR::ThemeManager::instance().color("color.meter.bar.fill"); }
inline QColor cSymbol() { return AetherSDR::ThemeManager::instance().color("color.text.secondary"); }
inline QColor cEdgeMarker() { return AetherSDR::ThemeManager::instance().color("color.text.secondary"); }
inline QColor cGrid() { return AetherSDR::theme::withAlpha("color.background.1", 150); }
inline QColor cInsetBg() { return AetherSDR::ThemeManager::instance().color("color.background.0"); }
inline QColor cInsetBorder() { return AetherSDR::ThemeManager::instance().color("color.border.subtle"); }
inline QColor cEmptyText() { return AetherSDR::ThemeManager::instance().color("color.text.disabled"); }

constexpr int kInnerPad = 3;       // gap between the inset border and content
constexpr int kSymbolLaneH = 14;   // decoded-symbol glyph lane
// Confidence lane height adapts to the widget height (height()/10) clamped to
// this band — a flat 8 px was too subtle to read at strip size.
constexpr int kConfLaneMinH = 14;
constexpr int kConfLaneMaxH = 28;
constexpr int kLaneGap = 2;        // vertical gap between lanes

} // namespace

ClockAlignmentWidget::ClockAlignmentWidget(QWidget* parent)
    : QWidget(parent)
{
    // We paint every pixel (inset fill + border), so skip the palette clear.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    // Dominant element of the applet (host lays it out with stretch 1).
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize ClockAlignmentWidget::sizeHint() const
{
    return {240, 150};
}

QSize ClockAlignmentWidget::minimumSizeHint() const
{
    return {220, 110};
}

void ClockAlignmentWidget::setWindowSeconds(int seconds)
{
    const int clamped = std::clamp(seconds, 5, 30);
    if (clamped == m_windowSeconds)
        return;
    m_windowSeconds = clamped;
    // Shrinking the window trims the oldest seconds off the front of the ring.
    while (m_frames.size() > m_windowSeconds)
        m_frames.removeFirst();
    update();
}

void ClockAlignmentWidget::appendFrame(const ClockAlignmentFrame& frame)
{
    m_frames.append(frame);
    while (m_frames.size() > m_windowSeconds)
        m_frames.removeFirst();
    update();
}

void ClockAlignmentWidget::clear()
{
    if (m_frames.isEmpty())
        return;
    m_frames.clear();
    update();
}

void ClockAlignmentWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);

    const QRect full = rect();

    // Resolve palette once per paint (a runtime theme change triggers a fresh
    // paintEvent, so these re-resolve; they are never cached across paints).
    const QColor insetBgCol = cInsetBg();
    const QColor insetBorderCol = cInsetBorder();
    const QColor envelopeCol = cEnvelope();
    const QColor expectedCol = cExpected();
    const QColor gridCol = cGrid();
    const QColor edgeCol = cEdgeMarker();
    const QColor symbolCol = cSymbol();
    const QColor confSuccessCol = cConfSuccess();
    const QColor confWarningCol = cConfWarning();
    const QColor confInactiveCol = cConfInactive();

    // Inset surface: fill then a 1 px border (style-guide inset pattern).
    p.fillRect(full, insetBgCol);
    p.setPen(QPen(insetBorderCol, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(full.adjusted(0, 0, -1, -1));

    if (m_frames.isEmpty()) {
        p.setPen(cEmptyText());
        p.drawText(full, Qt::AlignCenter, QStringLiteral("no signal"));
        return;
    }

    const QRect plot = full.adjusted(kInnerPad, kInnerPad, -kInnerPad, -kInnerPad);

    // Confidence lane grows with the widget so it stays legible at strip size.
    const int confLaneH = std::clamp(height() / 10, kConfLaneMinH, kConfLaneMaxH);

    // Vertical bands, measured up from the bottom: symbol lane, confidence
    // lane, then the scope lane takes the remaining height.
    const int symbolTop = plot.bottom() - kSymbolLaneH + 1;
    const int confBottom = symbolTop - kLaneGap;
    const int confTop = confBottom - confLaneH;
    const int scopeTop = plot.top();
    const int scopeBottom = std::max(scopeTop + 1, confTop - kLaneGap);
    const float scopeH = float(scopeBottom - scopeTop);

    const int window = m_windowSeconds;
    const int count = m_frames.size();
    const float colW = float(plot.width()) / float(window);

    // Shared normalization across the visible window so relative depth reads
    // correctly frame-to-frame. Include 0 in the range so envelope depth and a
    // zero-mean template share one consistent baseline.
    float vMax = 1e-6f;
    float vMin = 0.0f;
    for (const ClockAlignmentFrame& f : m_frames) {
        for (float s : f.envelope) {
            vMax = std::max(vMax, s);
            vMin = std::min(vMin, s);
        }
        for (float s : f.expected) {
            vMax = std::max(vMax, s);
            vMin = std::min(vMin, s);
        }
    }
    const float span = std::max(vMax - vMin, 1e-6f);
    const auto yOf = [&](float v) {
        return float(scopeBottom) - ((v - vMin) / span) * scopeH;
    };

    // A series (envelope or template) drawn across one second's column width on
    // its own x-scale, so mismatched envelope/expected sizes each map cleanly.
    const auto drawSeries = [&](const QVector<float>& s, const QColor& col,
                                float widthPx, float x0) {
        const int n = s.size();
        if (n == 0)
            return;
        QPen pen(col);
        pen.setWidthF(widthPx);
        pen.setCosmetic(true);
        p.setPen(pen);
        if (n == 1) {
            p.drawPoint(QPointF(x0 + colW * 0.5f, yOf(s[0])));
            return;
        }
        QPolygonF poly;
        poly.reserve(n);
        const float denom = float(n - 1);
        for (int k = 0; k < n; ++k) {
            const float x = x0 + (float(k) / denom) * colW;
            poly << QPointF(x, yOf(s[k]));
        }
        p.drawPolyline(poly);
    };

    // Grid + lane dividers: crisp, so antialiasing off.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(gridCol, 1));
    for (int i = 0; i <= window; ++i) {
        const float x = plot.left() + i * colW;
        p.drawLine(QPointF(x, scopeTop), QPointF(x, symbolTop + kSymbolLaneH - 1));
    }
    p.setPen(QPen(insetBorderCol, 1));
    p.drawLine(QPointF(plot.left(), confTop - 1),
               QPointF(plot.right(), confTop - 1));
    p.drawLine(QPointF(plot.left(), symbolTop - 1),
               QPointF(plot.right(), symbolTop - 1));

    QFont symFont = font();
    symFont.setPixelSize(10);
    symFont.setStyleHint(QFont::Monospace);
    symFont.setFamily(QStringLiteral("monospace"));

    const float edgeTickH = std::min(scopeH * 0.33f, 14.0f);

    for (int idx = 0; idx < count; ++idx) {
        const ClockAlignmentFrame& f = m_frames[idx];
        // Newest hugs the right edge; a short window sits blank on the left.
        const int col = window - count + idx;
        const float x0 = plot.left() + col * colW;

        // 1. + 2. Traces (antialiased). Envelope first, template over it. Empty
        // vectors simply skip the trace — the second's slot still advances.
        // The template is drawn shifted by edgeOffsetMs — where the matched
        // filter actually matched this second (WS-4.5): ~0 on a drift-free
        // stream, nonzero while the decoder absorbs sample-clock drift, so
        // envelope and template stay honest to each other on screen.
        if (!f.envelope.isEmpty() || !f.expected.isEmpty()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            drawSeries(f.envelope, envelopeCol, 1.2f, x0);
            const float shiftPx =
                (std::clamp(f.edgeOffsetMs, -500, 500) / 1000.0f) * colW;
            drawSeries(f.expected, expectedCol, 1.0f, x0 + shiftPx);
        }

        // 3. AM-drop edge tick — where the DETECTED second edge landed. The
        // engine emits edgeOffsetMs as SIGNED drift from the nominal window
        // start (~0 on a drift-free stream), so the tick takes the exact same
        // shift as the template overlay; reading it as an absolute 0..1000 ms
        // position pins the tick to the column edge and hides drift.
        if (!f.envelope.isEmpty()) {
            p.setRenderHint(QPainter::Antialiasing, false);
            const float frac = std::clamp(f.edgeOffsetMs, -500, 500) / 1000.0f;
            const float ex = x0 + frac * colW;
            p.setPen(QPen(edgeCol, 1));
            p.drawLine(QPointF(ex, float(scopeBottom)),
                       QPointF(ex, float(scopeBottom) - edgeTickH));
        }

        // 4. Confidence lane: bar height = margin, color by threshold.
        {
            p.setRenderHint(QPainter::Antialiasing, false);
            const float c = std::clamp(f.confidence, 0.0f, 1.0f);
            const QColor barCol = c >= 0.5f ? confSuccessCol
                                            : (c >= 0.25f ? confWarningCol : confInactiveCol);
            const float h = c * float(confLaneH);
            const QRectF bar(x0 + 1.0f, float(confBottom) - h,
                             std::max(colW - 2.0f, 1.0f), h);
            p.fillRect(bar, barCol);
        }

        // 5. Symbol glyph: 0/1 = decoded bit, 2 = frame marker, else unknown.
        {
            QChar glyph;
            switch (f.symbol) {
            case 0:
                glyph = QLatin1Char('0');
                break;
            case 1:
                glyph = QLatin1Char('1');
                break;
            case 2:
                glyph = QLatin1Char('M');
                break;
            default:
                glyph = QLatin1Char('?');
                break;
            }
            p.setFont(symFont);
            p.setPen(symbolCol);
            const QRectF cell(x0, float(symbolTop), colW, float(kSymbolLaneH));
            p.drawText(cell, Qt::AlignCenter, QString(glyph));
        }
    }
}

} // namespace AetherSDR
