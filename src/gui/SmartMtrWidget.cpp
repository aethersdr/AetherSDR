#include "SmartMtrWidget.h"

#include "SmartMtrStyle.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

namespace AetherSDR {

using namespace SmartMtrUnits;

namespace {
// Tick footprint (height away from the hole, thickness) for a marker size.
void markerExtent(MarkerSize size, double& height, double& width)
{
    if (size == MarkerSize::Large) {
        height = kMarkerLargeH;
        width = kMarkerLargeW;
    } else {
        height = kMarkerSmallH;
        width = kMarkerSmallW;
    }
}

const QColor& markerColor(MarkerColor color)
{
    return color == MarkerColor::High ? SmartMtrColors::kMarkerHigh
                                      : SmartMtrColors::kMarkerNormal;
}
} // namespace

SmartMtrWidget::SmartMtrWidget(QWidget* parent)
    : QWidget(parent)
{
    // Transparent so the VFO flag's painted background shows through the
    // letterbox margins around the fitted design.
    setAttribute(Qt::WA_TranslucentBackground);
    // The whole meter strip is one click target (toggles the selector); let
    // clicks fall through to VfoWidget::mousePressEvent instead of being eaten
    // by this widget.
    setAttribute(Qt::WA_TransparentForMouseEvents);

    setAccessibleName(tr("SmartMTR meter"));

    // Expand to all available parent width; height follows from heightForWidth()
    // so the control keeps its design aspect ratio at whatever width it gets.
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void SmartMtrWidget::setMeterInput(const MeterInput& input)
{
    m_input = input;
    update();
}

void SmartMtrWidget::paintEvent(QPaintEvent*)
{
    const auto g = SmartMtrGeometry::fit(rect());

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Back-to-front: body, recessed hole, indicator fill, the inset shadow rim
    // on top so the bar reads as sunken, then the scale markers/labels (drawn on
    // the body above and below the hole).
    drawControl(p, g);
    drawHole(p, g);
    drawIndicator(p, g);
    drawInsetShadow(p, g);
    drawMarkers(p, g);
}

void SmartMtrWidget::drawControl(QPainter& p, const SmartMtrGeometry& g) const
{
    p.fillRect(g.rect(0.0, 0.0, kControlW, kControlH), SmartMtrColors::kControl);
}

void SmartMtrWidget::drawHole(QPainter& p, const SmartMtrGeometry& g) const
{
    const QRectF hole = g.rect(kHoleMargX, kHoleMargY, kHoleW, kHoleH);
    const double r = g.len(kHoleRadius);
    p.setPen(Qt::NoPen);
    p.setBrush(SmartMtrColors::kBackground);
    p.drawRoundedRect(hole, r, r);
}

void SmartMtrWidget::drawIndicator(QPainter& p, const SmartMtrGeometry& g) const
{
    // Bar from the scale minimum to the mapped value position, full hole height.
    // Clipped to the rounded hole so the bar's corners follow the hole's radius.
    const QRectF hole = g.rect(kHoleMargX, kHoleMargY, kHoleW, kHoleH);
    const double r = g.len(kHoleRadius);
    QPainterPath clip;
    clip.addRoundedRect(hole, r, r);

    const double pos = indicatorPosition(m_input); // hole-local units, in band

    // The bar always starts at hole-local 0, so a min/blank value (pos == 10)
    // still renders a short 0..10 stub rather than nothing.
    p.save();
    p.setClipPath(clip);
    p.fillRect(g.rect(kHoleMargX, kHoleMargY, pos, kHoleH),
               SmartMtrColors::kForeground);
    // Bright value line at the bar's right end (the value), drawn just inside the
    // end so it never extends past the position.
    p.fillRect(g.rect(kHoleMargX + pos - kIndicatorLine, kHoleMargY,
                      kIndicatorLine, kHoleH),
               SmartMtrColors::kIndicator);
    p.restore();
}

void SmartMtrWidget::drawInsetShadow(QPainter& p, const SmartMtrGeometry& g) const
{
    // A kShadow-wide rim, equal on all four inner sides of the hole: the rounded
    // hole minus the same rounded rect deflated by kShadow on every side. The
    // inner radius shrinks by kShadow too, keeping the corners concentric.
    const QRectF outer = g.rect(kHoleMargX, kHoleMargY, kHoleW, kHoleH);
    const QRectF inner = g.rect(kHoleMargX + kShadow, kHoleMargY + kShadow,
                                kHoleW - 2.0 * kShadow, kHoleH - 2.0 * kShadow);
    const double ro = g.len(kHoleRadius);
    const double ri = g.len(kHoleRadius - kShadow);

    QPainterPath path;
    path.addRoundedRect(outer, ro, ro);
    path.addRoundedRect(inner, ri, ri);
    // Odd-even fill leaves only the rim (outer minus inner) painted.
    path.setFillRule(Qt::OddEvenFill);
    p.fillPath(path, SmartMtrColors::kShadow);
}

void SmartMtrWidget::drawMarkers(QPainter& p, const SmartMtrGeometry& g) const
{
    const MeterConfig& cfg = meterConfig(m_input.kind);
    if (cfg.markers.empty())
        return;

    // Label font: the app UI font, sized from kLabelHeight with a pixel floor so
    // it stays legible when the control is small.
    QFont labelFont = font();
    labelFont.setPixelSize(qMax(8, qRound(g.len(kLabelHeight))));
    const QFontMetricsF fm(labelFont);
    p.setFont(labelFont);

    const double holeBottom = kHoleMargY + kHoleH;

    for (const ScaleMarker& m : cfg.markers) {
        // Only ticks inside the scale band are rendered.
        if (m.position < kScaleMin || m.position > kScaleMax)
            continue;

        double h = 0.0, w = 0.0;
        markerExtent(m.size, h, w);
        const QColor& color = markerColor(m.color);
        const double x = kHoleMargX + m.position - w / 2.0; // centered on position

        // Symmetric pair, each stuck to a hole edge and growing outward.
        const QRectF above = g.rect(x, kHoleMargY - h, w, h);
        p.fillRect(above, color);
        p.fillRect(g.rect(x, holeBottom, w, h), color);

        if (m.label.isEmpty())
            continue;

        // Label centered on the marker (plus its per-marker offset), sitting
        // just above the top tick.
        const double cx = above.center().x() + g.len(m.labelOffset);
        const double bottom = above.top() - g.len(kLabelGap);
        const double tw = fm.horizontalAdvance(m.label);
        const double th = fm.height();
        const QRectF box(cx - tw, bottom - th, tw * 2.0, th);
        p.setPen(color);
        p.drawText(box, Qt::AlignHCenter | Qt::AlignBottom, m.label);
    }
}

QSize SmartMtrWidget::sizeHint() const
{
    // Baseline at the design size; the width is only a hint (the widget expands
    // to the parent), and the height is recomputed from the actual width via
    // heightForWidth().
    return QSize(qRound(kControlW), qRound(kControlH));
}

int SmartMtrWidget::heightForWidth(int width) const
{
    // Preserve the design aspect ratio so a full-width control is fully visible
    // (no vertical clipping) under the uniform UNITS->pixel scale.
    return qRound(width * (kControlH / kControlW));
}

} // namespace AetherSDR
