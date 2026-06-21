#include "SmartMtrWidget.h"

#include "SmartMtrStyle.h"

#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>

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

    // SmartMTR analog ballistics (ported from the SmartMTR macOS app): a fast
    // attack and a ~15x slower, lazy decay give the d'Arsonval "jumps up, sags
    // down" envelope-follower feel. tau values come from per-tick fractions
    // k=0.60/0.06 at 60 Hz: tau = -(1/60)/ln(1-k). The smoother integrates the
    // normalised scale fraction; a tiny snap epsilon keeps the slow tail lazy
    // (the source never snaps on decay) without endless sub-pixel repaints.
    MeterSmoother::Ballistics b;
    b.attackSeconds = 0.01818f;  // ~18.2 ms — fast rise
    b.releaseSeconds = 0.26940f; // ~269 ms — slow fall
    b.snapEpsilon = 0.0005f;
    m_smooth.setBallistics(b);

    m_animTimer.setTimerType(Qt::PreciseTimer);
    m_animTimer.setInterval(kMeterSmootherIntervalMs); // 8 ms ~= 120 Hz
    connect(&m_animTimer, &QTimer::timeout, this, &SmartMtrWidget::advance);

    // Free-running monotonic clock for the extremes window (sample timestamps and
    // slew dt). Kept separate from m_clock, which the smoother restarts each tick.
    m_extremesClock.start();
}

void SmartMtrWidget::setMeterInput(const MeterInput& input)
{
    const bool kindChanged = (input.kind != m_kind);
    m_input = input;
    m_kind = input.kind;

    // RX<->TX is a scale discontinuity (signal dBm vs mic dBFS); the extremes
    // window must not mix domains.
    if (kindChanged)
        m_extremes.reset();

    // Feed the raw sample into the extremes window (both kinds; mic uses it for
    // the AVG bar + PEAK marker). Skip on park (no value) or when disabled.
    if (m_extremesEnabled && input.hasValue)
        m_extremes.record(input.value, m_extremesClock.elapsed());

    // Bar target. For mic with extremes on, the bar tracks the windowed AVERAGE
    // (PEAK shown by the marker); otherwise it tracks the instantaneous value.
    const double posUnits =
        (m_extremesEnabled && input.hasValue && input.kind == MeterKind::MicLevel)
            ? mapRawToUnits(m_extremes.avgRaw())
            : indicatorPosition(input);

    // Normalise to the scale band so the ballistics are scale-independent.
    const double span = kScaleMax - kScaleMin;
    const float targetFrac =
        span > 0.0 ? float((posUnits - kScaleMin) / span) : 0.0f;

    m_smooth.setTarget(targetFrac);
    if (kindChanged)
        m_smooth.snapToTarget(); // snap across the discontinuity, don't glide

    // Keep the timer running while EITHER the bar or the extremes markers still
    // need to move (a peak can expire and slide a marker after the bar settles).
    if ((m_smooth.needsAnimation() || (m_extremesEnabled && m_extremes.hasData()))
        && !m_animTimer.isActive()) {
        m_clock.restart();
        m_animTimer.start();
    }
    update(); // paint the first frame promptly; the timer carries the rest
}

void SmartMtrWidget::advance()
{
    const qint64 dt = m_clock.restart();
    bool moving = m_smooth.tick(dt);

    if (m_extremesEnabled) {
        const bool extMoving = m_extremes.tick(
            m_extremesClock.elapsed(), dt, needlePosUnits(),
            [this](double raw) { return mapRawToUnits(raw); });
        moving = moving || extMoving;
    }

    const bool settled = !moving;
    if (settled)
        m_animTimer.stop();
    if (settled || m_smooth.shouldRepaint())
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
    drawExtremes(p, g);

    // Let the parent's value-label overlay repaint in lockstep with the markers.
    emit repainted();
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

    // Smoothed hole-local position: the ballistics integrate the normalised
    // scale fraction; map it back into the scale band here.
    const double pos = kScaleMin + m_smooth.value() * (kScaleMax - kScaleMin);

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
    // Soft inner shadow: each edge fades from the rim colour at the hole's edge
    // to fully transparent kShadow units inward, so the hole reads as a recess
    // rather than a flat border. Clipped to the rounded hole, so the gradients
    // follow the corners; the corner overlap deepens slightly, which looks
    // natural for a recess.
    const QRectF hole = g.rect(kHoleMargX, kHoleMargY, kHoleW, kHoleH);
    const double r = g.len(kHoleRadius);
    const double s = g.len(kShadow);
    if (s <= 0.0)
        return;

    QColor edge = SmartMtrColors::kShadow;
    QColor fade = edge;
    fade.setAlpha(0);

    QPainterPath clip;
    clip.addRoundedRect(hole, r, r);

    p.save();
    p.setClipPath(clip);
    p.setPen(Qt::NoPen);

    auto edgeGradient = [&](const QRectF& band, const QPointF& from, const QPointF& to) {
        QLinearGradient grad(from, to); // from = at the rim, to = s units inward
        grad.setColorAt(0.0, edge);
        grad.setColorAt(1.0, fade);
        p.fillRect(band, grad);
    };

    // Top / bottom / left / right inner edges.
    edgeGradient(QRectF(hole.left(), hole.top(), hole.width(), s),
                 QPointF(hole.left(), hole.top()), QPointF(hole.left(), hole.top() + s));
    edgeGradient(QRectF(hole.left(), hole.bottom() - s, hole.width(), s),
                 QPointF(hole.left(), hole.bottom()), QPointF(hole.left(), hole.bottom() - s));
    edgeGradient(QRectF(hole.left(), hole.top(), s, hole.height()),
                 QPointF(hole.left(), hole.top()), QPointF(hole.left() + s, hole.top()));
    edgeGradient(QRectF(hole.right() - s, hole.top(), s, hole.height()),
                 QPointF(hole.right(), hole.top()), QPointF(hole.right() - s, hole.top()));

    p.restore();
}

void SmartMtrWidget::drawMarkers(QPainter& p, const SmartMtrGeometry& g) const
{
    const MeterConfig& cfg = meterConfig(m_input.kind);
    if (cfg.markers.empty())
        return;

    const double holeBottom = kHoleMargY + kHoleH;

    for (const ScaleMarker& m : cfg.markers) {
        // Only ticks inside the scale band are rendered.
        if (m.position < kScaleMin || m.position > kScaleMax)
            continue;

        double h = 0.0, w = 0.0;
        markerExtent(m.size, h, w);
        const QColor& color = markerColor(m.color);
        const double x = kHoleMargX + m.position - w / 2.0; // centered on position

        // Small ticks read as secondary, so draw them a bit transparent.
        QColor tickColor = color;
        if (m.size == MarkerSize::Small)
            tickColor.setAlphaF(tickColor.alphaF() * kMarkerSmallOpacity);

        // Symmetric pair, each stuck to a hole edge and growing outward.
        const QRectF above = g.rect(x, kHoleMargY - h, w, h);
        p.fillRect(above, tickColor);
        p.fillRect(g.rect(x, holeBottom, w, h), tickColor);

        if (m.label.isEmpty())
            continue;

        // Per-label font: strong = full size + regular weight; normal = slightly
        // smaller + light weight. App UI font, with a pixel floor for legibility.
        const bool strong = (m.labelStyle == LabelStyle::Strong);
        QFont labelFont = font();
        labelFont.setPixelSize(
            qMax(8, qRound(g.len(strong ? kLabelHeight : kLabelHeightNormal))));
        labelFont.setWeight(strong ? QFont::Normal : QFont::Light);
        p.setFont(labelFont);
        const QFontMetricsF fm(labelFont);

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

void SmartMtrWidget::setExtremesOptions(bool show, ExtremesSpeed speed,
                                        MeterValues values)
{
    m_extremesEnabled = show;
    m_showValues = values;

    MeterExtremes::Tuning t;
    switch (speed) {
    case ExtremesSpeed::Slow:
        t.windowSeconds = SmartMtrExtremes::kWindowSlowSec;
        break;
    case ExtremesSpeed::Fast:
        t.windowSeconds = SmartMtrExtremes::kWindowFastSec;
        break;
    case ExtremesSpeed::Medium:
        t.windowSeconds = SmartMtrExtremes::kWindowMediumSec;
        break;
    }
    t.slewUnitsPerSec = SmartMtrExtremes::kSlewUnitsPerSec;
    m_extremes.setTuning(t);

    if (!show)
        m_extremes.reset();
    update();
}

double SmartMtrWidget::mapRawToUnits(double raw) const
{
    const double pos = meterConfig(m_kind).valueToPosition(raw, m_input.min, m_input.max);
    return std::clamp(pos, kScaleMin, kScaleMax);
}

double SmartMtrWidget::needlePosUnits() const
{
    return kScaleMin + double(m_smooth.value()) * (kScaleMax - kScaleMin);
}

bool SmartMtrWidget::extremesActive() const
{
    return m_extremesEnabled && m_extremes.hasData();
}

double SmartMtrWidget::signalFade() const
{
    // Mic (dBFS) has no dBm floor → always full. Signal: fade out near the floor.
    if (m_kind == MeterKind::MicLevel)
        return 1.0;
    const double cur = m_input.hasValue ? m_input.value : SmartMtrExtremes::kSignalFadeLoDbm;
    return std::clamp(
        (cur - SmartMtrExtremes::kSignalFadeLoDbm)
            / (SmartMtrExtremes::kSignalFadeHiDbm - SmartMtrExtremes::kSignalFadeLoDbm),
        0.0, 1.0);
}

double SmartMtrWidget::extremesOpacity() const
{
    // Mic: the lone PEAK marker has no trough to compare against and the dBFS
    // scale has no dBm floor, so it shows at full opacity.
    if (m_kind == MeterKind::MicLevel)
        return 1.0;

    // Signal: proximity fade (min/max too close) x signal fade (near-floor).
    const double spread = m_extremes.maxRaw() - m_extremes.minRaw();
    const double prox = std::clamp(
        (spread - SmartMtrExtremes::kFadeLoDb)
            / (SmartMtrExtremes::kFadeHiDb - SmartMtrExtremes::kFadeLoDb),
        0.0, 1.0);
    return prox * signalFade();
}

QString SmartMtrWidget::extremeSUnit(double raw) const
{
    // S-units only make sense for the RX signal scale (S9 = -73 dBm, 6 dB each,
    // S0 = -127). Above S9, report the overshoot as "+NdB".
    if (m_kind != MeterKind::Signal)
        return QString();
    if (raw > -73.0)
        return QStringLiteral("+%1dB").arg(qRound(raw + 73.0));
    // floor, not round: report the S-unit actually reached (e.g. -86 dBm is just
    // shy of S7 at -85, so it reads s6, not s7).
    const int s = std::clamp(int(std::floor((raw + 127.0) / 6.0)), 0, 9);
    return QStringLiteral("s%1").arg(s);
}

QString SmartMtrWidget::extremeDbm(double raw) const
{
    const int v = qRound(raw);
    return m_kind == MeterKind::MicLevel ? QStringLiteral("%1dB").arg(v)
                                         : QStringLiteral("%1dBm").arg(v);
}

void SmartMtrWidget::drawExtremes(QPainter& p, const SmartMtrGeometry& g) const
{
    if (!extremesActive())
        return;
    const double opacity = extremesOpacity();
    if (opacity < 0.02)
        return;

    QColor c = SmartMtrColors::kExtreme;
    c.setAlphaF(c.alphaF() * opacity);

    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(c);

    // Summit (tip) at the top, stuck to the hole's top edge; body hangs down
    // INSIDE the hole. The tip marks the exact position on the scale.
    auto drawTri = [&](double posUnits) {
        const double x = kHoleMargX + posUnits;             // hole-local unit X
        const double apexY = kHoleMargY;                    // summit on hole top edge
        const double baseY = kHoleMargY + SmartMtrExtremes::kExtremeTriH; // base inside hole
        const double halfW = SmartMtrExtremes::kExtremeTriW / 2.0;
        QPolygonF tri;
        tri << g.point(x, apexY) << g.point(x - halfW, baseY)
            << g.point(x + halfW, baseY);
        p.drawPolygon(tri);
    };

    // MAX (peak) for both kinds; MIN (trough) for signal only.
    drawTri(m_extremes.maxPosUnits());
    if (m_kind == MeterKind::Signal)
        drawTri(m_extremes.minPosUnits());

    p.restore();
}

QVector<SmartMtrWidget::ExtremeMarker> SmartMtrWidget::extremeLabels() const
{
    QVector<ExtremeMarker> out;

    // Two lines for signal (S-unit over dBm); a single top line for mic (no
    // S-unit), so the value isn't left floating on the lower line.
    auto marker = [&](double raw, double pos, double opacity, bool isMax) {
        const QString s = extremeSUnit(raw);
        const QString d = extremeDbm(raw);
        return s.isEmpty() ? ExtremeMarker{ pos, d, QString(), opacity, isMax }
                           : ExtremeMarker{ pos, s, d, opacity, isMax };
    };

    if (m_showValues == MeterValues::Signal) {
        // Current signal value at the live needle position, rendered like the MAX
        // marker (line + label to its right). Always fully visible when selected —
        // the fade-out on small min/max spread applies only to the Extremes mode.
        if (m_input.hasValue)
            out.push_back(marker(m_input.value, needlePosUnits(), 1.0, true));
        return out;
    }

    if (m_showValues == MeterValues::Extremes && extremesActive()) {
        const double opacity = extremesOpacity();
        if (opacity < 0.02)
            return out;
        // MAX (peak) — both kinds.
        out.push_back(marker(m_extremes.maxRaw(), m_extremes.maxPosUnits(), opacity, true));
        // MIN (trough) — signal only.
        if (m_kind == MeterKind::Signal)
            out.push_back(
                marker(m_extremes.minRaw(), m_extremes.minPosUnits(), opacity, false));
    }
    return out;
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
