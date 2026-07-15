#include "CrossNeedleMeterWidget.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QAccessibleWidget>
#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLoggingCategory>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <QRadialGradient>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionFocusRect>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcCrossNeedleMeter, "aether.gui.crossneedle")

namespace {

constexpr double kDesignAspect = 1.5;
constexpr double kForwardFullScale = 20.0;
constexpr double kReflectedFullScale = 4.0;
constexpr MeterSmoother::Ballistics kNeedleBallistics{0.075f, 0.250f, 0.0005f};

class CrossNeedleMeterAccessible : public QAccessibleWidget {
  public:
    explicit CrossNeedleMeterAccessible(QWidget *widget)
        : QAccessibleWidget(widget, QAccessible::Indicator) {}

    QString text(QAccessible::Text textType) const override {
        const CrossNeedleMeterWidget *meter =
            qobject_cast<const CrossNeedleMeterWidget *>(widget());
        if (meter && textType == QAccessible::Value) {
            return meter->accessibleValueText();
        }
        return QAccessibleWidget::text(textType);
    }
};

QAccessibleInterface *crossNeedleMeterFactory(const QString &key, QObject *object) {
    if (key == QLatin1String("AetherSDR::CrossNeedleMeterWidget")) {
        return new CrossNeedleMeterAccessible(qobject_cast<QWidget *>(object));
    }
    return nullptr;
}

QPointF pointOnRadius(const QPointF &center, double radius, double angle) {
    return center + QPointF(std::cos(angle), std::sin(angle)) * radius;
}

QPainterPath sampledArc(const QPointF &center, double radius, double startRadians,
                        double endRadians) {
    constexpr int kSamples = 180;
    QPainterPath path;
    for (int i = 0; i < kSamples; ++i) {
        const double fraction = static_cast<double>(i) / (kSamples - 1);
        const double angle = startRadians + fraction * (endRadians - startRadians);
        const QPointF point = pointOnRadius(center, radius, angle);
        if (i == 0) {
            path.moveTo(point);
        } else {
            path.lineTo(point);
        }
    }
    return path;
}

const QImage &paperGrainTexture() {
    // Keep the texture near the meter's nominal on-screen size. The previous
    // 750 x 500 field averaged several noise samples into every applet pixel,
    // making its grain disappear when the design was reduced to 260 x 173.
    // Two correlated noise bands and sparse flecks read as paper/paint fibres
    // without introducing a repeating raster asset.
    static const QImage texture = []() {
        // Roughly 1.4 logical applet pixels per texel at the nominal size:
        // broad enough to remain visible on a Retina capture, fine enough not
        // to look like discrete digital blocks.
        QImage image(180, 120, QImage::Format_RGB32);
        quint32 state = 0x6d2b79f5U;
        const auto nextNoise = [&state]() {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            return state;
        };
        for (int y = 0; y < image.height(); ++y) {
            const int rowBias = static_cast<int>((nextNoise() >> 28U) & 0x0fU) - 8;
            int horizontalFibre = 0;
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const quint32 sample = nextNoise();
                const int fine = static_cast<int>((sample >> 26U) & 0x3fU) - 32;
                horizontalFibre = (7 * horizontalFibre + fine) / 8;
                const int fleck = (sample & 0xffU) < 6U
                    ? (((sample >> 8U) & 1U) == 0U ? -34 : 34)
                    : 0;
                const int value = std::clamp(
                    128 + rowBias + fine + 2 * horizontalFibre + fleck, 58, 198);
                line[x] = qRgb(value, value, value);
            }
        }
        return image;
    }();
    return texture;
}

void drawPaperGrain(QPainter &painter, const QRectF &face, double opacity) {
    painter.save();
    painter.setOpacity(opacity);
    // Overlay keeps a mid-grey texture luminance-neutral while giving its
    // darker and lighter fibres enough contrast to survive at 260 px wide.
    painter.setCompositionMode(QPainter::CompositionMode_Overlay);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(face, paperGrainTexture());
    painter.restore();
}

} // namespace

CrossNeedleMeterWidget::CrossNeedleMeterWidget(QWidget *parent)
    : QWidget(parent), m_forwardSmoother(kNeedleBallistics),
      m_reflectedSmoother(kNeedleBallistics) {
    static bool s_accessibilityFactoryInstalled = false;
    if (!s_accessibilityFactoryInstalled) {
        s_accessibilityFactoryInstalled = true;
        QAccessible::installFactory(crossNeedleMeterFactory);
    }

    QString geometryError;
    m_geometry = CrossNeedleMeterGeometry::loadResource(&geometryError);
    if (!m_geometry.isValid()) {
        qCWarning(lcCrossNeedleMeter).noquote()
            << "CrossNeedleMeterWidget: using degraded fallback geometry:" << geometryError;
        m_geometry = CrossNeedleMeterGeometry::fallback();
    }

    setObjectName(QStringLiteral("crossNeedleMeter"));
    setAccessibleName(tr("Cross-needle power and SWR meter"));
    setAccessibleDescription(tr("Transmit meter with separate forward and reflected power needles; "
                                "their crossing indicates standing wave ratio"));
    setFocusPolicy(Qt::TabFocus);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAttribute(Qt::WA_OpaquePaintEvent);

    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    m_animationTimer.setTimerType(Qt::PreciseTimer);
    m_animationTimer.setInterval(kMeterSmootherIntervalMs);
    connect(&m_animationTimer, &QTimer::timeout, this, &CrossNeedleMeterWidget::advanceNeedles);

    m_accessibilityTimer.setSingleShot(true);
    m_accessibilityTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_accessibilityTimer, &QTimer::timeout, this,
            &CrossNeedleMeterWidget::publishAccessibleValue);

    updateTargets(true);
    publishAutomationProperties();
}

QSize CrossNeedleMeterWidget::sizeHint() const { return QSize(260, heightForWidth(260)); }

QSize CrossNeedleMeterWidget::minimumSizeHint() const { return QSize(210, heightForWidth(210)); }

int CrossNeedleMeterWidget::heightForWidth(int width) const {
    return qRound(static_cast<double>(width) / kDesignAspect);
}

QString CrossNeedleMeterWidget::faceThemeId() const {
    switch (m_faceTheme) {
    case FaceTheme::DarkRoomUplight:
        return QStringLiteral("dark-room-uplight");
    case FaceTheme::GraphiteDark:
        return QStringLiteral("graphite-dark");
    case FaceTheme::ClassicWarm:
        return QStringLiteral("classic-warm");
    }
    return QStringLiteral("classic-warm");
}

void CrossNeedleMeterWidget::setFaceTheme(FaceTheme theme) {
    if (m_faceTheme == theme) {
        return;
    }
    m_faceTheme = theme;
    m_cacheValid = false;
    publishAutomationProperties();
    update();
}

QString CrossNeedleMeterWidget::accessibleValueText() const {
    if (!m_transmitting && !m_automationFixture) {
        return tr("Receive, forward and reflected needles parked at zero");
    }
    return tr("Transmit, forward %1 watts, reflected %2 watts, SWR %3, range "
              "times %4")
        .arg(m_forwardWatts, 0, 'f', 1)
        .arg(m_reflectedWatts, 0, 'f', 1)
        .arg(m_swr, 0, 'f', 2)
        .arg(m_rangeMultiplier, 0, 'f', 0);
}

void CrossNeedleMeterWidget::setTxMeters(float forwardWatts, float swr) {
    if (m_automationFixture || !std::isfinite(forwardWatts) || !std::isfinite(swr)) {
        return;
    }

    m_forwardWatts = std::max(0.0, static_cast<double>(forwardWatts));
    m_swr = std::max(1.0, static_cast<double>(swr));
    m_reflectedWatts = CrossNeedleMeterGeometry::reflectedPowerWatts(m_forwardWatts, m_swr);
    m_reflectedPowerMeasured = false;
    updateTargets(false);
}

void CrossNeedleMeterWidget::setTxPowers(float forwardWatts, float reflectedWatts) {
    if (m_automationFixture || !std::isfinite(forwardWatts) || !std::isfinite(reflectedWatts)) {
        return;
    }

    m_forwardWatts = std::max(0.0, static_cast<double>(forwardWatts));
    m_reflectedWatts = std::max(0.0, static_cast<double>(reflectedWatts));
    m_swr = CrossNeedleMeterGeometry::swrFromPowers(m_forwardWatts, m_reflectedWatts);
    m_reflectedPowerMeasured = true;
    updateTargets(false);
}

void CrossNeedleMeterWidget::setTransmitting(bool transmitting) {
    if (m_automationFixture) {
        return;
    }
    m_transmitting = transmitting;
    updateTargets(false);
}

void CrossNeedleMeterWidget::setPowerScale(int maxWatts, bool amplifierActive) {
    const double multiplier =
        CrossNeedleMeterGeometry::rangeMultiplierFor(maxWatts, amplifierActive);
    if (qFuzzyCompare(multiplier, m_rangeMultiplier)) {
        return;
    }
    m_rangeMultiplier = multiplier;
    updateTargets(true);
}

void CrossNeedleMeterWidget::setAutomationFixture(float forwardWatts, float swr) {
    if (!qEnvironmentVariableIsSet("AETHER_AUTOMATION") || !std::isfinite(forwardWatts) ||
        !std::isfinite(swr)) {
        return;
    }
    m_automationFixture = true;
    m_transmitting = true;
    m_forwardWatts = std::max(0.0, static_cast<double>(forwardWatts));
    m_swr = std::max(1.0, static_cast<double>(swr));
    m_reflectedWatts = CrossNeedleMeterGeometry::reflectedPowerWatts(m_forwardWatts, m_swr);
    m_reflectedPowerMeasured = false;
    updateTargets(false);
}

void CrossNeedleMeterWidget::setAutomationPowerFixture(float forwardWatts, float reflectedWatts) {
    if (!qEnvironmentVariableIsSet("AETHER_AUTOMATION") || !std::isfinite(forwardWatts) ||
        !std::isfinite(reflectedWatts)) {
        return;
    }
    m_automationFixture = true;
    m_transmitting = true;
    m_forwardWatts = std::max(0.0, static_cast<double>(forwardWatts));
    m_reflectedWatts = std::max(0.0, static_cast<double>(reflectedWatts));
    m_swr = CrossNeedleMeterGeometry::swrFromPowers(m_forwardWatts, m_reflectedWatts);
    m_reflectedPowerMeasured = true;
    updateTargets(false);
}

void CrossNeedleMeterWidget::clearAutomationFixture() {
    if (!qEnvironmentVariableIsSet("AETHER_AUTOMATION")) {
        return;
    }
    m_automationFixture = false;
    m_transmitting = false;
    m_forwardWatts = 0.0;
    m_reflectedWatts = 0.0;
    m_swr = 1.0;
    m_reflectedPowerMeasured = false;
    updateTargets(false);
}

void CrossNeedleMeterWidget::updateTargets(bool snap) {
    // Interlock state is authoritative. Meter UDP and interlock TCP updates
    // arrive on independent worker threads, so a late power packet after RX
    // must update the cache without bringing the needles back off their stops.
    const bool active = m_transmitting || m_automationFixture;
    const double multiplier = std::max(1.0, m_rangeMultiplier);
    const float forwardTarget =
        active ? static_cast<float>(
                     std::clamp(m_forwardWatts / (kForwardFullScale * multiplier), 0.0, 1.0))
               : 0.0f;
    const float reflectedTarget =
        active ? static_cast<float>(
                     std::clamp(m_reflectedWatts / (kReflectedFullScale * multiplier), 0.0, 1.0))
               : 0.0f;

    m_forwardSmoother.setTarget(forwardTarget);
    m_reflectedSmoother.setTarget(reflectedTarget);
    if (snap) {
        m_forwardSmoother.snapToTarget();
        m_reflectedSmoother.snapToTarget();
    }

    if ((m_forwardSmoother.needsAnimation() || m_reflectedSmoother.needsAnimation()) &&
        !m_animationTimer.isActive()) {
        m_animationClock.restart();
        m_animationTimer.start();
    }
    publishAutomationProperties();
    scheduleAccessibleValue();
    update();
}

void CrossNeedleMeterWidget::advanceNeedles() {
    const qint64 elapsed = m_animationClock.restart();
    const bool forwardMoving = m_forwardSmoother.tick(elapsed);
    const bool reflectedMoving = m_reflectedSmoother.tick(elapsed);
    if (!forwardMoving && !reflectedMoving) {
        m_animationTimer.stop();
    }
    publishAutomationProperties();
    update();
}

QRectF CrossNeedleMeterWidget::fittedDesignRect() const {
    const double availableWidth = width();
    const double availableHeight = height();
    const double fittedWidth = std::min(availableWidth, availableHeight * kDesignAspect);
    const double fittedHeight = fittedWidth / kDesignAspect;
    return QRectF((availableWidth - fittedWidth) / 2.0, (availableHeight - fittedHeight) / 2.0,
                  fittedWidth, fittedHeight);
}

void CrossNeedleMeterWidget::applyDesignTransform(QPainter &painter) const {
    const QRectF fitted = fittedDesignRect();
    painter.translate(fitted.topLeft());
    painter.scale(fitted.width() / m_geometry.canvasWidth,
                  fitted.height() / m_geometry.canvasHeight);
}

void CrossNeedleMeterWidget::drawCenteredText(QPainter &painter, const QPointF &center,
                                              const QString &text) const {
    const QFontMetricsF metrics(painter.font());
    const QRectF box = metrics.boundingRect(text);
    painter.drawText(center.x() - box.width() / 2.0, center.y() + metrics.ascent() / 2.0, text);
}

void CrossNeedleMeterWidget::drawCenteredMultilineText(QPainter &painter, const QPointF &center,
                                                       const QString &text) const {
    const QStringList lines = text.split(QLatin1Char('\n'));
    const QFontMetricsF metrics(painter.font());
    const double lineHeight = metrics.height();
    const double firstCenterY =
        center.y() - lineHeight * (static_cast<double>(lines.size()) - 1.0) / 2.0;
    for (int i = 0; i < lines.size(); ++i) {
        drawCenteredText(painter, QPointF(center.x(), firstCenterY + i * lineHeight), lines[i]);
    }
}

void CrossNeedleMeterWidget::drawRotatedText(QPainter &painter, const QPointF &center,
                                             const QString &text, double degrees) const {
    painter.save();
    painter.translate(center);
    // QPainter's positive rotation is clockwise in its y-down coordinate
    // system, matching the V12 proof generator's image-space convention.
    painter.rotate(degrees);
    drawCenteredText(painter, QPointF(), text);
    painter.restore();
}

void CrossNeedleMeterWidget::drawScale(QPainter &painter,
                                       const CrossNeedleMeterGeometry::Scale &scale,
                                       const CrossNeedleMeterGeometry::Title &title) const {
    const CrossNeedleMeterGeometry::ScaleStyle &style = m_geometry.scaleStyle;
    const CrossNeedleMeterGeometry::DarkTheme &dark = m_geometry.darkTheme;
    const bool graphiteDark = m_faceTheme == FaceTheme::GraphiteDark;
    const QColor outer = graphiteDark ? dark.scaleOuter : style.outer;
    const QColor separator = graphiteDark
                                 ? dark.scaleSeparator
                                 : (m_faceTheme == FaceTheme::DarkRoomUplight
                                        ? m_geometry.uplightGradient.scaleSeparator
                                        : style.separator);
    const QColor calibration = graphiteDark ? dark.scaleCalibration : style.calibration;
    const QColor inner = graphiteDark ? dark.scaleInner : style.inner;
    const QColor majorTick = graphiteDark ? dark.majorTick : style.majorTick;
    const QColor minorTick = graphiteDark ? dark.minorTick : style.minorTick;
    const QColor text = graphiteDark ? dark.text : style.text;

    // drawFace() leaves the face-coloured brush active. An open QPainterPath
    // is implicitly closed for filling, so retaining that brush would paint a
    // large face-coloured chord across the rear scale. Scale arcs are strokes
    // only; explicitly clearing the brush keeps the reflected graph continuous.
    painter.setBrush(Qt::NoBrush);
    // The calibration card is built from flat printed-ink layers. At the
    // applet's 260 px nominal width these widths resolve to a visible cream
    // ribbon, charcoal frame, warm separator, burgundy calibration rule, and
    // slate inner rule without relying on raster artwork or changing an arc.
    if (m_faceTheme == FaceTheme::ClassicWarm) {
        painter.setPen(
            QPen(style.ribbon, style.ribbonWidth, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        painter.drawPath(sampledArc(scale.center, scale.radius - style.ribbonInset,
                                    scale.startRadians, scale.endRadians));
    }
    painter.setPen(QPen(outer, style.outerWidth, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.drawPath(sampledArc(scale.center, scale.radius, scale.startRadians, scale.endRadians));
    painter.setPen(
        QPen(separator, style.separatorWidth, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.drawPath(sampledArc(scale.center, scale.radius - style.separatorInset,
                                scale.startRadians, scale.endRadians));
    painter.setPen(
        QPen(calibration, style.calibrationWidth, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.drawPath(sampledArc(scale.center, scale.radius - style.calibrationInset,
                                scale.startRadians, scale.endRadians));
    painter.setPen(QPen(inner, style.innerWidth, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.drawPath(sampledArc(scale.center, scale.radius - style.innerInset, scale.startRadians,
                                scale.endRadians));

    const QPen minorPen(minorTick, style.minorTickWidth, Qt::SolidLine, Qt::SquareCap);
    for (int i = 0; i + 1 < scale.anglesRadians.size(); ++i) {
        const double first = scale.anglesRadians[i];
        const double second = scale.anglesRadians[i + 1];
        for (int subdivision = 1; subdivision < scale.minorSubdivisions; ++subdivision) {
            const double fraction = static_cast<double>(subdivision) / scale.minorSubdivisions;
            const double angle = first + (second - first) * fraction;
            const QPointF radial(std::cos(angle), std::sin(angle));
            const QPointF point = scale.center + radial * scale.radius;
            painter.setPen(minorPen);
            painter.drawLine(point - radial * 11.0, point + radial * 8.0);
        }
    }

    QFont numberFont = font();
    numberFont.setPixelSize(m_geometry.typography.scaleNumberPixels);
    numberFont.setWeight(QFont::Medium);
    painter.setFont(numberFont);
    painter.setPen(QPen(majorTick, style.majorTickWidth, Qt::SolidLine, Qt::SquareCap));
    for (int i = 0; i < scale.anglesRadians.size(); ++i) {
        const double angle = scale.anglesRadians[i];
        const QPointF radial(std::cos(angle), std::sin(angle));
        const QPointF point = scale.center + radial * scale.radius;
        painter.drawLine(point - radial * 20.0, point + radial * 14.0);
        if (!scale.labels[i].isEmpty()) {
            painter.save();
            painter.setPen(text);
            drawCenteredText(painter, point + radial * scale.labelOffset, scale.labels[i]);
            painter.restore();
        }
    }

    QFont titleFont = font();
    titleFont.setPixelSize(m_geometry.typography.sideTitlePixels);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(text);
    drawRotatedText(painter, title.center, title.text, title.rotationDegrees);
}

void CrossNeedleMeterWidget::drawSwrGuides(QPainter &painter) const {
    const bool graphiteDark = m_faceTheme == FaceTheme::GraphiteDark;
    const QColor guideColor =
        graphiteDark ? m_geometry.darkTheme.swrGuide : m_geometry.swrStyle.guide;
    const QColor labelColor =
        graphiteDark ? m_geometry.darkTheme.swrLabel : m_geometry.swrStyle.label;
    painter.setPen(QPen(guideColor, m_geometry.swrStyle.guideWidth, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    for (const CrossNeedleMeterGeometry::SwrGuide &guide : m_geometry.swrGuides) {
        painter.drawPath(m_geometry.swrGuidePath(guide));
    }

    QFont labelFont = font();
    labelFont.setPixelSize(m_geometry.typography.swrNumberPixels);
    labelFont.setBold(true);
    painter.setFont(labelFont);
    for (const CrossNeedleMeterGeometry::SwrGuide &guide : m_geometry.swrGuides) {
        if (guide.displayLabel.isEmpty()) {
            continue;
        }
        const QString display = guide.displayLabel == QStringLiteral("infinity")
                                    ? QString::fromUtf8("\xe2\x88\x9e")
                                    : guide.displayLabel;
        const QFontMetricsF metrics(labelFont);
        const QRectF textRect = metrics.boundingRect(display).adjusted(-3.0, -2.0, 3.0, 2.0);
        QRectF background = textRect;
        background.moveCenter(m_geometry.swrGuideLabelCenter(guide));
        const CrossNeedleMeterGeometry::Frame &frame = m_geometry.frame;
        const QRectF face(frame.faceInset, frame.faceInset,
                          m_geometry.canvasWidth - 2.0 * frame.faceInset,
                          m_geometry.canvasHeight - 2.0 * frame.faceInset);
        painter.save();
        painter.setClipRect(background);
        drawFaceBackground(painter, face);
        painter.restore();
        painter.setPen(labelColor);
        drawCenteredText(painter, m_geometry.swrGuideLabelCenter(guide), display);
    }
}

void CrossNeedleMeterWidget::drawFaceBackground(QPainter &painter, const QRectF &face) const {
    if (m_faceTheme == FaceTheme::GraphiteDark) {
        const CrossNeedleMeterGeometry::DarkTheme &dark = m_geometry.darkTheme;

        QLinearGradient card(face.topLeft(), face.bottomLeft());
        card.setColorAt(0.0, dark.top);
        card.setColorAt(dark.middleStop, dark.middle);
        card.setColorAt(1.0, dark.bottom);
        painter.fillRect(face, card);

        QRadialGradient ambient(dark.ambientCenter, dark.ambientRadius);
        ambient.setColorAt(0.0, dark.ambientInner);
        ambient.setColorAt(1.0, dark.ambientOuter);
        painter.fillRect(face, ambient);

        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        QRadialGradient glow(dark.glowCenter, dark.glowRadius);
        glow.setColorAt(0.0, dark.glowInner);
        glow.setColorAt(dark.glowMiddleStop, dark.glowMiddle);
        glow.setColorAt(1.0, dark.glowOuter);
        painter.fillRect(face, glow);
        painter.restore();

        QColor clearEdge = dark.vignetteEdge;
        clearEdge.setAlpha(0);
        QRadialGradient vignette(dark.vignetteCenter, dark.vignetteRadius);
        vignette.setColorAt(0.0, clearEdge);
        vignette.setColorAt(dark.vignetteClearStop, clearEdge);
        vignette.setColorAt(1.0, dark.vignetteEdge);
        painter.fillRect(face, vignette);

        drawPaperGrain(painter, face, dark.paperGrainOpacity);
        return;
    }

    if (m_faceTheme == FaceTheme::DarkRoomUplight) {
        const CrossNeedleMeterGeometry::UplightGradient &light = m_geometry.uplightGradient;

        // The theme is deliberately composed from ordinary QPainter layers:
        // low ambient exposure, a broad lamp halo, a concentrated bulb glow,
        // a small paper-diffusion bloom, then a symmetric edge vignette. The
        // two inner layers use Screen blending to behave like emitted light
        // passing through the card rather than opaque orange paint.
        QLinearGradient ambient(face.topLeft(), face.bottomLeft());
        ambient.setColorAt(0.0, light.top);
        ambient.setColorAt(light.middleStop, light.middle);
        ambient.setColorAt(1.0, light.bottom);
        painter.fillRect(face, ambient);

        QRadialGradient halo(light.haloCenter, light.haloRadius);
        halo.setColorAt(0.0, light.haloInner);
        halo.setColorAt(light.haloMiddleStop, light.haloMiddle);
        halo.setColorAt(light.haloShoulderStop, light.haloShoulder);
        halo.setColorAt(1.0, light.haloOuter);
        painter.fillRect(face, halo);

        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        QRadialGradient hotspot(light.hotspotCenter, light.hotspotRadius);
        hotspot.setColorAt(0.0, light.hotspotInner);
        hotspot.setColorAt(light.hotspotMiddleStop, light.hotspotMiddle);
        hotspot.setColorAt(1.0, light.hotspotOuter);
        painter.fillRect(face, hotspot);

        QRadialGradient bloom(light.bloomCenter, light.bloomRadius);
        bloom.setColorAt(0.0, light.bloomInner);
        bloom.setColorAt(light.bloomMiddleStop, light.bloomMiddle);
        bloom.setColorAt(1.0, light.bloomOuter);
        painter.fillRect(face, bloom);
        painter.restore();

        QColor clearEdge = light.vignetteEdge;
        clearEdge.setAlpha(0);
        QRadialGradient vignette(light.vignetteCenter, light.vignetteRadius);
        vignette.setColorAt(0.0, clearEdge);
        vignette.setColorAt(light.vignetteClearStop, clearEdge);
        vignette.setColorAt(1.0, light.vignetteEdge);
        painter.fillRect(face, vignette);

        drawPaperGrain(painter, face, light.paperGrainOpacity);
        return;
    }

    const CrossNeedleMeterGeometry::FaceGradient &material = m_geometry.faceGradient;

    QLinearGradient base(face.topLeft(), face.bottomLeft());
    base.setColorAt(0.0, material.top);
    base.setColorAt(material.middleStop, material.middle);
    base.setColorAt(1.0, material.bottom);
    painter.fillRect(face, base);

    QRadialGradient glow(material.glowCenter, material.glowRadius);
    glow.setColorAt(0.0, material.glowInner);
    glow.setColorAt(1.0, material.glowOuter);
    painter.fillRect(face, glow);

    QColor clearEdge = material.vignetteEdge;
    clearEdge.setAlpha(0);
    QRadialGradient vignette(material.vignetteCenter, material.vignetteRadius);
    vignette.setColorAt(0.0, clearEdge);
    vignette.setColorAt(material.vignetteClearStop, clearEdge);
    vignette.setColorAt(1.0, material.vignetteEdge);
    painter.fillRect(face, vignette);
}

void CrossNeedleMeterWidget::drawFace(QPainter &painter) const {
    const CrossNeedleMeterGeometry::Frame &frame = m_geometry.frame;
    const QRectF outer(frame.outerInset, frame.outerInset,
                       m_geometry.canvasWidth - 2.0 * frame.outerInset,
                       m_geometry.canvasHeight - 2.0 * frame.outerInset);
    painter.setPen(Qt::NoPen);
    painter.setBrush(frame.bezel);
    painter.drawRoundedRect(outer, frame.outerRadius, frame.outerRadius);

    const QRectF face(frame.faceInset, frame.faceInset,
                      m_geometry.canvasWidth - 2.0 * frame.faceInset,
                      m_geometry.canvasHeight - 2.0 * frame.faceInset);
    QPainterPath facePath;
    facePath.addRoundedRect(face, frame.faceRadius, frame.faceRadius);
    painter.save();
    painter.setClipPath(facePath);
    drawFaceBackground(painter, face);
    painter.restore();
    painter.setPen(QPen(frame.bezelEdge, frame.faceOutlineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(face, frame.faceRadius, frame.faceRadius);

    // Both baselines stay continuous. Paint order alone makes the reflected
    // graph pass behind the Forward graph at their upper crossing. A narrow
    // mask local to the reflected trajectory makes the underpass readable.
    if (m_geometry.scaleOverlap.reflectedBehindForward) {
        drawScale(painter, m_geometry.reflectedScale, m_geometry.reflectedTitle);
        const CrossNeedleMeterGeometry::ScaleOverlap &overlap = m_geometry.scaleOverlap;
        const QPainterPath gapCenterline =
            sampledArc(m_geometry.reflectedScale.center,
                       m_geometry.reflectedScale.radius - overlap.reflectedGapCenterRadiusInset,
                       overlap.reflectedGapCenterRadians - overlap.reflectedGapHalfSpanRadians,
                       overlap.reflectedGapCenterRadians + overlap.reflectedGapHalfSpanRadians);
        QPainterPathStroker gapStroker;
        gapStroker.setWidth(overlap.reflectedGapWidth);
        gapStroker.setCapStyle(Qt::FlatCap);
        gapStroker.setJoinStyle(Qt::RoundJoin);
        painter.save();
        painter.setClipPath(gapStroker.createStroke(gapCenterline));
        drawFaceBackground(painter, face);
        painter.restore();
        drawScale(painter, m_geometry.forwardScale, m_geometry.forwardTitle);
    } else {
        drawScale(painter, m_geometry.forwardScale, m_geometry.forwardTitle);
        drawScale(painter, m_geometry.reflectedScale, m_geometry.reflectedTitle);
    }

    QFont rangeFont = font();
    rangeFont.setPixelSize(m_geometry.typography.rangePixels);
    rangeFont.setBold(true);
    painter.setFont(rangeFont);
    painter.setPen(m_faceTheme == FaceTheme::GraphiteDark ? m_geometry.darkTheme.rangeText
                                                          : QColor(54, 61, 66));
    drawCenteredMultilineText(painter, m_geometry.rangeLabelCenter, m_geometry.rangeLabel);

    QFont unitFont = font();
    unitFont.setPixelSize(m_geometry.typography.unitPixels);
    unitFont.setBold(true);
    painter.setFont(unitFont);
    painter.setPen(m_faceTheme == FaceTheme::GraphiteDark ? m_geometry.darkTheme.text
                                                          : m_geometry.scaleStyle.text);
    drawCenteredText(painter, m_geometry.forwardUnitCenter, QStringLiteral("(W)"));
    drawCenteredText(painter, m_geometry.reflectedUnitCenter, QStringLiteral("(W)"));

    // The reference's SWR ink sits above the printed power graphs. Its lower
    // continuation is intentionally present here and concealed by the mask.
    painter.save();
    QPainterPath faceClip;
    faceClip.addRoundedRect(face, frame.faceRadius, frame.faceRadius);
    painter.setClipPath(faceClip);
    drawSwrGuides(painter);
    painter.restore();
}

void CrossNeedleMeterWidget::drawLowerMask(QPainter &painter) const {
    const bool graphiteDark = m_faceTheme == FaceTheme::GraphiteDark;
    const QColor fill = graphiteDark ? m_geometry.darkTheme.maskFill : m_geometry.mask.fill;
    const QColor edge = graphiteDark ? m_geometry.darkTheme.maskEdge : m_geometry.mask.edge;
    const QColor text = graphiteDark ? m_geometry.darkTheme.maskText : m_geometry.mask.text;
    QPolygonF polygon;
    polygon.append(QPointF(m_geometry.mask.boundary.first().x(), m_geometry.mask.bottomY));
    for (const QPointF &point : m_geometry.mask.boundary) {
        polygon.append(point);
    }
    polygon.append(QPointF(m_geometry.mask.boundary.last().x(), m_geometry.mask.bottomY));
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawPolygon(polygon);

    painter.setPen(QPen(edge, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolyline(QPolygonF(m_geometry.mask.boundary));

    QFont labelFont = font();
    labelFont.setPixelSize(m_geometry.typography.maskLabelPixels);
    labelFont.setBold(true);
    painter.setFont(labelFont);
    painter.setPen(text);
    drawCenteredText(painter, m_geometry.mask.labelCenter, m_geometry.mask.label);
}

void CrossNeedleMeterWidget::rebuildStaticLayer() {
    const qreal dpr = devicePixelRatioF();
    const QSize pixelSize(qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
    m_staticLayer = QPixmap(pixelSize);
    m_staticLayer.setDevicePixelRatio(dpr);
    m_staticLayer.fill(Qt::transparent);

    QPainter painter(&m_staticLayer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    applyDesignTransform(painter);
    drawFace(painter);

    m_cacheSize = size();
    m_cacheDpr = dpr;
    m_cacheValid = true;
}

void CrossNeedleMeterWidget::drawNeedles(QPainter &painter) const {
    const double forwardDisplay = m_forwardSmoother.value() * kForwardFullScale * m_rangeMultiplier;
    const double reflectedDisplay =
        m_reflectedSmoother.value() * kReflectedFullScale * m_rangeMultiplier;
    const QPointF forwardTip = m_geometry.forwardTip(forwardDisplay, m_rangeMultiplier);
    const QPointF reflectedTip = m_geometry.reflectedTip(reflectedDisplay, m_rangeMultiplier);

    const QRectF face(m_geometry.frame.faceInset, m_geometry.frame.faceInset,
                      m_geometry.canvasWidth - 2.0 * m_geometry.frame.faceInset,
                      m_geometry.canvasHeight - 2.0 * m_geometry.frame.faceInset);
    QPainterPath clip;
    clip.addRoundedRect(face, m_geometry.frame.faceRadius, m_geometry.frame.faceRadius);
    painter.save();
    painter.setClipPath(clip);

    const bool graphiteDark = m_faceTheme == FaceTheme::GraphiteDark;
    const CrossNeedleMeterGeometry::NeedleStyle &style = m_geometry.needleStyle;
    const QColor shadow = graphiteDark ? m_geometry.darkTheme.needleShadow : style.shadow;
    const QColor line = graphiteDark ? m_geometry.darkTheme.needle : style.line;
    const QColor edge = graphiteDark ? m_geometry.darkTheme.needleEdge : style.edge;
    const QColor highlight =
        graphiteDark ? m_geometry.darkTheme.needleHighlight : style.highlight;
    const auto drawMovement = [&painter, &style, shadow, line, edge, highlight](
                                  const QPointF &pivot, const QPointF &tip) {
        // Build physical depth from code-drawn material layers. Only the soft
        // shadows and sub-pixel edge reflections are displaced; the body is
        // always painted on the calibrated pivot-to-tip ray. The cast shadows
        // remain continuous over the printed scale, as they would on a
        // physical meter face beneath an elevated needle.
        painter.setPen(QPen(style.softShadow, style.softShadowWidth, Qt::SolidLine,
                            Qt::RoundCap));
        painter.drawLine(pivot + style.softShadowOffset, tip + style.softShadowOffset);
        painter.setPen(QPen(shadow, style.shadowWidth, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(pivot + style.shadowOffset, tip + style.shadowOffset);

        painter.setPen(QPen(line, style.lineWidth, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(pivot, tip);

        const QPointF movement = tip - pivot;
        const double length = std::hypot(movement.x(), movement.y());
        if (length > 0.0) {
            QPointF upperLeftNormal(-movement.y() / length, movement.x() / length);
            if (QPointF::dotProduct(upperLeftNormal, QPointF(-1.0, -1.0)) < 0.0) {
                upperLeftNormal = -upperLeftNormal;
            }
            const QPointF highlightOffset = upperLeftNormal * style.highlightOffset;
            const QPointF edgeOffset = -upperLeftNormal * style.edgeOffset;
            painter.setPen(QPen(edge, style.edgeWidth, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(pivot + edgeOffset, tip + edgeOffset);
            painter.setPen(
                QPen(highlight, style.highlightWidth, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(pivot + highlightOffset, tip + highlightOffset);
        }
    };
    drawMovement(m_geometry.forwardScale.center, forwardTip);
    drawMovement(m_geometry.reflectedScale.center, reflectedTip);
    painter.restore();
}

void CrossNeedleMeterWidget::paintEvent(QPaintEvent *) {
    const qreal dpr = devicePixelRatioF();
    if (!m_cacheValid || m_cacheSize != size() || !qFuzzyCompare(m_cacheDpr, dpr)) {
        rebuildStaticLayer();
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    painter.drawPixmap(0, 0, m_staticLayer);
    painter.save();
    applyDesignTransform(painter);
    drawNeedles(painter);
    drawLowerMask(painter);
    painter.restore();

    if (hasFocus()) {
        QStyleOptionFocusRect option;
        option.initFrom(this);
        option.rect = rect().adjusted(2, 2, -2, -2);
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, this);
    }
}

void CrossNeedleMeterWidget::changeEvent(QEvent *event) {
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        m_cacheValid = false;
        update();
    }
    QWidget::changeEvent(event);
}

void CrossNeedleMeterWidget::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Menu ||
        (event->key() == Qt::Key_F10 && event->modifiers().testFlag(Qt::ShiftModifier))) {
        emit customContextMenuRequested(rect().center());
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CrossNeedleMeterWidget::scheduleAccessibleValue() {
    if (hasFocus() && QAccessible::isActive() && !m_accessibilityTimer.isActive()) {
        m_accessibilityTimer.start();
    }
}

void CrossNeedleMeterWidget::publishAccessibleValue() {
    if (!hasFocus() || !QAccessible::isActive()) {
        return;
    }
    QAccessibleValueChangeEvent event(this, accessibleValueText());
    QAccessible::updateAccessibility(&event);
}

void CrossNeedleMeterWidget::publishAutomationProperties() {
    if (!qEnvironmentVariableIsSet("AETHER_AUTOMATION")) {
        return;
    }
    const QPointF intersection =
        m_geometry.needleIntersection(m_forwardWatts, m_reflectedWatts, m_rangeMultiplier);
    double guideDistance = 0.0;
    const QString guide = m_geometry.nearestGuideLabel(intersection, &guideDistance);
    const double displayedForwardWatts =
        m_forwardSmoother.value() * kForwardFullScale * m_rangeMultiplier;
    const double displayedReflectedWatts =
        m_reflectedSmoother.value() * kReflectedFullScale * m_rangeMultiplier;

    setProperty("meterStyle", QStringLiteral("cross-needle"));
    setProperty("faceTheme", faceThemeId());
    setProperty("geometryDesignVersion", m_geometry.designVersion);
    setProperty("forwardWatts", m_forwardWatts);
    setProperty("reflectedWatts", m_reflectedWatts);
    setProperty("reflectedPowerSource",
                m_reflectedPowerMeasured ? QStringLiteral("measured") : QStringLiteral("derived"));
    setProperty("swr", m_swr);
    setProperty("rangeMultiplier", m_rangeMultiplier);
    setProperty("transmitting", m_transmitting);
    setProperty("effectiveActive", m_transmitting || m_automationFixture);
    setProperty("automationFixture", m_automationFixture);
    setProperty("forwardAngleRadians", m_geometry.forwardAngle(m_forwardWatts, m_rangeMultiplier));
    setProperty("reflectedAngleRadians",
                m_geometry.reflectedAngle(m_reflectedWatts, m_rangeMultiplier));
    setProperty("intersectionX", intersection.x());
    setProperty("intersectionY", intersection.y());
    setProperty("nearestSwrGuide", guide);
    setProperty("nearestGuideDistancePx", guideDistance);
    setProperty("displayedForwardWatts", displayedForwardWatts);
    setProperty("displayedReflectedWatts", displayedReflectedWatts);
    setProperty("displayedForwardAngleRadians",
                m_geometry.forwardAngle(displayedForwardWatts, m_rangeMultiplier));
    setProperty("displayedReflectedAngleRadians",
                m_geometry.reflectedAngle(displayedReflectedWatts, m_rangeMultiplier));
    setProperty("needleAnimationActive", m_animationTimer.isActive());
}

} // namespace AetherSDR
