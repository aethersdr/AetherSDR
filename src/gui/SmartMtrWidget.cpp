#include "SmartMtrWidget.h"

#include "SmartMtrStyle.h"

#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

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

    // Seed the active config so the value->position mapping is valid before the
    // first setMeterInput (markers stay empty until then; drawMarkers no-ops), and
    // so applyBallistics can read its reversed flag.
    m_activeCfg = meterConfig(m_kind);

    // Ballistics depend on the meter kind (signal vs mic) — see applyBallistics.
    // Seed with the default kind's set; setMeterInput re-applies on a kind switch.
    applyBallistics(m_kind);

    m_animTimer.setTimerType(Qt::PreciseTimer);
    m_animTimer.setInterval(kMeterSmootherIntervalMs); // 8 ms ~= 120 Hz
    connect(&m_animTimer, &QTimer::timeout, this, &SmartMtrWidget::advance);

    // Free-running monotonic clock for the extremes window (sample timestamps and
    // slew dt). Kept separate from m_clock, which the smoother restarts each tick.
    m_extremesClock.start();
}

void SmartMtrWidget::applyBallistics(MeterKind kind)
{
    MeterSmoother::Ballistics b;
    if (kind == MeterKind::MicLevel) {
        // PPM / audio-meter feel: track the live mic level as tightly as the mic
        // packet rate allows so speech reads as a lively, twitchy bar. Both tau
        // are well under the ~50-100 ms gap between mic packets, so the bar
        // effectively lands on each new sample instead of lagging behind a
        // falling value. The d'Arsonval lazy sag below is right for an S-meter
        // but feels dead on voice; the PEAK extremes marker handles peak-hold.
        b.attackSeconds = 0.002f;  // ~2 ms — essentially instant rise
        b.releaseSeconds = 0.045f; // ~45 ms — fast fall, just enough to not flicker
    } else {
        // SmartMTR analog ballistics (ported from the SmartMTR macOS app): a fast
        // attack and a ~15x slower, lazy decay give the d'Arsonval "jumps up, sags
        // down" envelope-follower feel. tau values come from per-tick fractions
        // k=0.60/0.06 at 60 Hz: tau = -(1/60)/ln(1-k).
        b.attackSeconds = 0.01818f;  // ~18.2 ms — fast rise
        b.releaseSeconds = 0.26940f; // ~269 ms — slow fall
    }
    // Reversed (gain-reduction) meters fill as the smoothed fraction FALLS, so the
    // smoother's "attack" (rising) and "release" (falling) map to the opposite
    // ends of the bar's motion. Swap them so the BAR keeps the conventional
    // fast-attack / slow-decay feel — jumps toward peak compression and recedes
    // slowly — instead of crawling up and snapping back.
    if (m_activeCfg.reversed) {
        const float tmp = b.attackSeconds;
        b.attackSeconds = b.releaseSeconds;
        b.releaseSeconds = tmp;
    }
    // The smoother integrates the normalised scale fraction; a tiny snap epsilon
    // keeps the slow tail lazy (the source never snaps on decay) without endless
    // sub-pixel repaints.
    b.snapEpsilon = 0.0005f;
    m_smooth.setBallistics(b);
}

void SmartMtrWidget::setMeterInput(const MeterInput& input)
{
    // A non-finite value (NaN/Inf) would survive std::clamp (both comparisons
    // false), stick in the bar smoother so needsAnimation() never clears, and
    // poison the extremes running sum permanently. Drop it; the last good frame
    // stays on screen until a finite reading arrives.
    if (input.hasValue && !std::isfinite(input.value))
        return;
    if (input.hasPeak && !std::isfinite(input.peak))
        return;

    const bool kindChanged = (input.kind != m_kind);
    // Power's config is radio-aware (its markers depend on input.max); the rest
    // are static. Rebuild the active config only when it can actually change — a
    // kind switch, or a Power full-scale change (radio swap) — not on every meter
    // packet, since buildPowerConfig() does real work (log/sort/label formatting).
    const bool powerScaleChanged = input.kind == MeterKind::Power
        && (input.min != m_input.min || input.max != m_input.max);
    m_input = input;
    m_kind = input.kind;

    if (kindChanged || powerScaleChanged)
        m_activeCfg = (input.kind == MeterKind::Power) ? buildPowerConfig(input.max)
                                                       : meterConfig(input.kind);

    // RX<->TX is a scale discontinuity (signal dBm vs mic dBFS); the extremes
    // window must not mix domains, and the bar ballistics differ per kind.
    if (kindChanged) {
        m_extremes.setReversed(m_activeCfg.reversed);
        m_extremes.reset();
        applyBallistics(m_kind);
    }

    // Drive the extremes engine. A kind with an externally-measured peak (mic's
    // MICPEAK, forward-power's instant sample, compression's peak) overrides the
    // MAX marker directly; the rest (signal, SWR) derive min/max from a local
    // sliding window of the value stream. Skip on park (no value) or when disabled.
    if (m_extremesEnabled && input.hasValue) {
        if (input.hasPeak)
            m_extremes.setExternalPeak(input.peak);
        else
            m_extremes.record(input.value, m_extremesClock.elapsed());
    }

    // Bar target = the instantaneous value. (Mic used to track the windowed
    // AVERAGE here for a VU-like read, but a 1-5s mean barely twitches on speech
    // and read as "dead"; the mic now uses a snappy PPM ballistic on the live
    // level instead — see applyBallistics — with the PEAK marker still holding
    // the loud peaks.)
    const double posUnits = indicatorPosition(input);

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
    const bool barMoving = m_smooth.tick(dt);
    bool moving = barMoving;

    // A returning marker must repaint at a smooth cadence even when the bar's
    // lean gate throttles to 12 Hz (the bar is settled during the glide, so its
    // gate would step the markers). Gate the marker repaint on its own clock at
    // kExtremesRepaintHz, bypassing the bar's lean gate only while a marker moves.
    bool extremesRepaintDue = false;
    if (m_extremesEnabled) {
        const qint64 now = m_extremesClock.elapsed();
        const bool extMoving = m_extremes.tick(
            now, dt, needlePosUnits(),
            [this](double raw) { return mapRawToUnits(raw); });
        moving = moving || extMoving;
        if (extMoving) {
            constexpr qint64 kGateMs = 1000 / SmartMtrExtremes::kExtremesRepaintHz;
            if (m_lastExtremesRepaintMs < 0
                || now - m_lastExtremesRepaintMs >= kGateMs) {
                m_lastExtremesRepaintMs = now;
                extremesRepaintDue = true;
            }
        }
    }

    const bool settled = !moving;
    if (settled)
        m_animTimer.stop();
    // Repaint on: the settled (final) frame; a gated marker step; or the bar
    // moving (through its lean gate). Gate the bar repaint on barMoving so the
    // timer staying alive through a marker hold (nothing moving) doesn't repaint.
    if (settled || extremesRepaintDue || (barMoving && m_smooth.shouldRepaint()))
        update();
}

void SmartMtrWidget::changeEvent(QEvent* e)
{
    // The cached marker layer (m_aboveBar) bakes label glyphs built from font(),
    // but the cache key is only {size, kind, dpr}. A theme/app font change would
    // otherwise leave stale glyphs until the next resize/kind/DPR change, so
    // invalidate the static layers here and repaint.
    if (e->type() == QEvent::FontChange
        || e->type() == QEvent::ApplicationFontChange
        || e->type() == QEvent::StyleChange) {
        m_cacheValid = false;
        update();
    }
    QWidget::changeEvent(e);
}

void SmartMtrWidget::paintEvent(QPaintEvent*)
{
    const auto g = SmartMtrGeometry::fit(rect());

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Back-to-front: body + recessed hole (cached), indicator fill, the inset
    // shadow rim + scale markers/labels on top (cached) so the bar reads as
    // sunken. Only the indicator bar and the extremes markers move frame-to-
    // frame; the rest is static for a given size/kind/DPR, so it's blitted from
    // two pixmaps and rebuilt only when that key changes (see
    // rebuildStaticLayers) — keeping the hot repaint cheap over the GPU panadapter.
    const qreal dpr = devicePixelRatioF();
    if (!m_cacheValid || m_cacheSize != size() || m_cacheKind != m_input.kind
        || m_cacheDpr != dpr || m_cacheMin != m_input.min || m_cacheMax != m_input.max)
        rebuildStaticLayers(g);

    p.drawPixmap(0, 0, m_belowBar);
    drawIndicator(p, g);
    p.drawPixmap(0, 0, m_aboveBar);
    drawExtremes(p, g);

    // Let the parent's value-label overlay repaint in lockstep with the markers.
    emit repainted();
}

void SmartMtrWidget::rebuildStaticLayers(const SmartMtrGeometry& g)
{
    const qreal dpr = devicePixelRatioF();
    const QSize logical = size();
    if (logical.isEmpty())
        return; // nothing to fit yet; stay invalid and rebuild once sized

    // Backing stores at device resolution so the cached layers stay crisp on
    // HiDPI; the geometry stays in logical units (g already maps through rect()).
    auto make = [&](QPixmap& pm) {
        pm = QPixmap(logical * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
    };
    make(m_belowBar);
    make(m_aboveBar);

    {
        QPainter bp(&m_belowBar);
        bp.setRenderHint(QPainter::Antialiasing, true);
        drawControl(bp, g);
        drawHole(bp, g);
        // Type label sits in the below-bar layer: above the hole background but
        // below the indicator bar (which is painted on top in paintEvent), so the
        // bar covers it where they overlap.
        drawTypeLabel(bp, g);
    }
    {
        QPainter ap(&m_aboveBar);
        ap.setRenderHint(QPainter::Antialiasing, true);
        drawInsetShadow(ap, g);
        drawMarkers(ap, g);
    }

    m_cacheSize = logical;
    m_cacheDpr = dpr;
    m_cacheKind = m_input.kind;
    m_cacheMin = m_input.min;
    m_cacheMax = m_input.max;
    m_cacheValid = true;
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
    if (m_activeCfg.reversed) {
        // Reversed (gain-reduction) fill: mirror of the normal bar. The normal bar
        // is anchored at the hole's LEFT edge (hole-local 0) and grows to pos; this
        // one is anchored at the hole's RIGHT edge (kHoleW) and grows leftward to
        // pos. So a min/blank value renders a short stub at the right edge (like
        // the normal bar's 0..kScaleMin stub), and rising compression fills toward
        // -25 — anchored at the hole end, not at the "0" value position.
        p.fillRect(g.rect(kHoleMargX + pos, kHoleMargY, kHoleW - pos, kHoleH),
                   SmartMtrColors::kForeground);
        // Bright value line at the moving (left) edge of the fill.
        p.fillRect(g.rect(kHoleMargX + pos, kHoleMargY, kIndicatorLine, kHoleH),
                   SmartMtrColors::kIndicator);
    } else {
        p.fillRect(g.rect(kHoleMargX, kHoleMargY, pos, kHoleH),
                   SmartMtrColors::kForeground);
        // Bright value line at the bar's right end (the value), drawn just inside
        // the end so it never extends past the position.
        p.fillRect(g.rect(kHoleMargX + pos - kIndicatorLine, kHoleMargY,
                          kIndicatorLine, kHoleH),
                   SmartMtrColors::kIndicator);
    }
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
    const MeterConfig& cfg = m_activeCfg;
    if (cfg.markers.empty())
        return;

    const double holeBottom = kHoleMargY + kHoleH;

    // Every label is anchored at the LARGE tick height so a small labeled tick's
    // label sits on the same baseline row as the large ones (only the tick stays
    // shorter). Its opacity still follows the tick, so a small tick's label reads
    // as secondary too.
    double largeH = 0.0, largeW = 0.0;
    markerExtent(MarkerSize::Large, largeH, largeW);

    // Labels come in two fixed styles (strong = full size + regular weight,
    // normal = slightly smaller + light); build each font and its metrics once
    // rather than per labeled tick. App UI font, with a pixel floor for legibility.
    auto makeLabelFont = [&](bool strong) {
        QFont f = font();
        f.setPixelSize(
            qMax(8, qRound(g.len(strong ? kLabelHeight : kLabelHeightNormal))));
#ifdef Q_OS_WIN
        // DirectWrite renders Light/Normal very thin at these small pixel sizes;
        // nudge each style up one step so Windows matches the weight macOS/Linux
        // already show. macOS/Linux keep Light/Normal.
        f.setWeight(strong ? QFont::Medium : QFont::Normal);
#else
        f.setWeight(strong ? QFont::Normal : QFont::Light);
#endif
        return f;
    };
    const QFont strongFont = makeLabelFont(true);
    const QFont normalFont = makeLabelFont(false);
    const QFontMetricsF strongFm(strongFont);
    const QFontMetricsF normalFm(normalFont);
    // A font-uniform vertical anchor: cap height (the height of capitals/digits
    // above the baseline). It's a DECLARED font metric, not a rasterized
    // measurement, so it's identical regardless of hinting/AA backend — unlike
    // tightBoundingRect, whose measured ink FreeType can report inconsistently.
    // Digits span [baseline-capHeight .. baseline], so their visual centre sits
    // capHeight/2 above the baseline; that lets strong (S9) and normal labels be
    // co-centred on one line on every platform.
    const double strongCap = strongFm.capHeight();
    const double normalCap = normalFm.capHeight();

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

        // Pick the prebuilt font/metrics for this label's style.
        const bool strong = (m.labelStyle == LabelStyle::Strong);
        const QFont& labelFont = strong ? strongFont : normalFont;
        const QFontMetricsF& fm = strong ? strongFm : normalFm;
        p.setFont(labelFont);

        // Label centered on the marker (plus its per-marker offset), sitting just
        // above the LARGE tick height (shared baseline) regardless of this tick's
        // own size.
        const double cx = above.center().x() + g.len(m.labelOffset);
        const double labelTop = g.rect(x, kHoleMargY - largeH, w, largeH).top();
        const double bottom = labelTop - g.len(kLabelGap);
        // Center every label on the regular-label center line so a strong (larger)
        // label is vertically centered with the regular ones, rather than sharing
        // their bottom baseline (which would push its taller glyphs upward).
        const double centerY = bottom - normalFm.height() / 2.0;
        // Anchor the baseline so the cap/digit band is centred on that line.
        // Cap height is declared (backend-independent), so all same-style labels
        // share one baseline and strong/normal stay co-centred on every platform
        // — unlike AlignVCenter, which centres the font line box whose
        // ascent/descent split differs across Core Text / FreeType / DirectWrite
        // and left S9 top-aligned on Linux/Windows.
        const double cap = strong ? strongCap : normalCap;
        const double baseline = centerY + cap / 2.0;
        // Label fades with the tick: a small (secondary) tick gets a dimmed label.
        p.setPen(tickColor);
        p.drawText(QPointF(cx - fm.horizontalAdvance(m.label) / 2.0, baseline), m.label);
    }
}

void SmartMtrWidget::drawTypeLabel(QPainter& p, const SmartMtrGeometry& g) const
{
    // TX meters only — the RX signal meter shows no type label.
    if (!m_showTypeLabel || m_kind == MeterKind::Signal)
        return;

    QString text;
    switch (m_kind) {
    case MeterKind::MicLevel:    text = QStringLiteral("MIC");  break;
    case MeterKind::SWR:         text = QStringLiteral("SWR");  break;
    case MeterKind::Power:       text = QStringLiteral("PWR");  break;
    case MeterKind::Compression: text = QStringLiteral("COMP"); break;
    case MeterKind::Signal:      return;
    }

    // Fit the font within the hole height so the (uppercase) label never overflows
    // the recess; clipped to the hole as a hard guarantee at any flag size.
    QFont f = font();
    f.setPixelSize(qMax(7, qRound(g.len(kHoleH * 0.8))));
    f.setWeight(QFont::Medium);
    p.setFont(f);

    QColor c(255, 255, 255);
    c.setAlphaF(0.6); // white, 60% opacity
    p.setPen(c);

    // Bound the label to the marker span [kScaleMin, kScaleMax] so it never sits
    // further out than the last marker (right, normal) or the first marker (left,
    // reversed compression). Clip to the hole as a hard guarantee.
    const QRectF hole = g.rect(kHoleMargX, kHoleMargY, kHoleW, kHoleH);
    const QRectF box = g.rect(kHoleMargX + kScaleMin, kHoleMargY,
                              kScaleMax - kScaleMin, kHoleH);
    const Qt::Alignment align = Qt::AlignVCenter
        | (m_activeCfg.reversed ? Qt::AlignLeft : Qt::AlignRight);
    p.save();
    p.setClipRect(hole);
    p.drawText(box, align, text);
    p.restore();
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

void SmartMtrWidget::setShowTypeLabel(bool on)
{
    if (m_showTypeLabel == on)
        return;
    m_showTypeLabel = on;
    m_cacheValid = false; // the label is baked into the cached below-bar layer
    update();
}

double SmartMtrWidget::mapRawToUnits(double raw) const
{
    const double pos = m_activeCfg.valueToPosition(raw, m_input.min, m_input.max);
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
    // Only the RX signal scale has a dBm floor to fade toward; every TX scale
    // (mic dBFS, SWR, power, compression) reads at full strength.
    if (m_kind != MeterKind::Signal)
        return 1.0;
    const double cur = m_input.hasValue ? m_input.value : SmartMtrExtremes::kSignalFadeLoDbm;
    return std::clamp(
        (cur - SmartMtrExtremes::kSignalFadeLoDbm)
            / (SmartMtrExtremes::kSignalFadeHiDbm - SmartMtrExtremes::kSignalFadeLoDbm),
        0.0, 1.0);
}

double SmartMtrWidget::extremesOpacity() const
{
    // TX scales show a lone PEAK marker with no trough to compare against and no
    // dBm floor, so they show at full opacity. Only signal gets the proximity fade.
    if (m_kind != MeterKind::Signal)
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
    // Above S9, report the overshoot as "+NdB" — but only once it rounds to at
    // least +1: a value just past -73 dBm rounds to "+0dB", which should read
    // "s9", not a meaningless zero overshoot. Sub-1 dB overshoots fall through to
    // the S-unit branch below, which yields s9 near -73.
    const int over = qRound(raw + 73.0);
    if (over >= 1)
        return QStringLiteral("+%1dB").arg(over);
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

    // Peak marker: normally the window/external MAX. For a reversed (gain-
    // reduction) meter the peak is the window MIN — the most compression — drawn
    // toward the -25 end. MIN trough is additionally drawn for signal only.
    if (m_activeCfg.reversed) {
        drawTri(m_extremes.minPosUnits());
    } else {
        drawTri(m_extremes.maxPosUnits());
        if (m_kind == MeterKind::Signal)
            drawTri(m_extremes.minPosUnits());
    }

    p.restore();
}

QVector<SmartMtrWidget::ExtremeMarker> SmartMtrWidget::extremeLabels() const
{
    QVector<ExtremeMarker> out;

    // Numeric value labels are a signal-meter feature only; other kinds (mic/TX)
    // show no value text (the peak marker itself still draws).
    if (m_kind != MeterKind::Signal)
        return out;

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
        // Same opacity as the triangle markers (drawExtremes) so they fade and
        // disappear together.
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
