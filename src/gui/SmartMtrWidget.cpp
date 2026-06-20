#include "SmartMtrWidget.h"

#include "SmartMtrStyle.h"

#include <QPainter>
#include <QPainterPath>

namespace AetherSDR {

using namespace SmartMtrUnits;

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

void SmartMtrWidget::paintEvent(QPaintEvent*)
{
    const auto g = SmartMtrGeometry::fit(rect());

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Back-to-front: body, recessed hole, indicator fill, then the inset shadow
    // rim on top so the bar reads as sunken.
    drawControl(p, g);
    drawHole(p, g);
    drawIndicator(p, g);
    drawInsetShadow(p, g);
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
    // Left-to-right bar, full hole height, kIndicatorFraction of the hole width.
    // Clipped to the rounded hole so the bar's corners follow the hole's radius.
    const QRectF hole = g.rect(kHoleMargX, kHoleMargY, kHoleW, kHoleH);
    const double r = g.len(kHoleRadius);
    QPainterPath clip;
    clip.addRoundedRect(hole, r, r);

    const double barW = kIndicatorFraction * kHoleW;

    p.save();
    p.setClipPath(clip);
    p.fillRect(g.rect(kHoleMargX, kHoleMargY, barW, kHoleH),
               SmartMtrColors::kForeground);
    // Bright marker line on top of the bar's right end, right-aligned so it sits
    // inside the bar (its last kIndicatorLine units), never extending past it.
    p.fillRect(g.rect(kHoleMargX + barW - kIndicatorLine, kHoleMargY,
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
