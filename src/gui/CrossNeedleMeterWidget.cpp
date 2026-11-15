#include "CrossNeedleMeterWidget.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QAccessibleWidget>
#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
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

    QString themeError;
    m_faceThemes = AnalogMeterFaceThemeCatalog::loadResource(&themeError);
    if (!m_faceThemes.isValid()) {
        qCWarning(lcCrossNeedleMeter).noquote()
            << "CrossNeedleMeterWidget: using fallback face themes:" << themeError;
        m_faceThemes = AnalogMeterFaceThemeCatalog::fallback();
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

AnalogMeterFaceTheme CrossNeedleMeterWidget::analogFaceTheme() const {
    switch (m_faceTheme) {
    case FaceTheme::ClassicWarm:
        return AnalogMeterFaceTheme::ClassicWarm;
    case FaceTheme::DarkRoomUplight:
        return AnalogMeterFaceTheme::DarkRoomUplight;
    case FaceTheme::GraphiteDark:
        return AnalogMeterFaceTheme::GraphiteDark;
    }
    return AnalogMeterFaceTheme::ClassicWarm;
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

void CrossNeedleMeterWidget::setRangeLegendVisible(bool visible) {
    if (m_rangeLegendVisible == visible) {
        return;
    }
    m_rangeLegendVisible = visible;
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
    const AnalogMeterFaceThemeCatalog::Palette &colors =
        m_faceThemes.palette(analogFaceTheme());

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
            QPen(colors.ribbon, style.ribbonWidth, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        painter.drawPath(sampledArc(scale.center, scale.radius - style.ribbonInset,
                                    scale.startRadians, scale.endRadians));
    }
    painter.setPen(QPen(colors.scaleOuter, style.outerWidth, Qt::SolidLine, Qt::FlatCap,
                        Qt::RoundJoin));
    painter.drawPath(sampledArc(scale.center, scale.radius, scale.startRadians, scale.endRadians));
    painter.setPen(
        QPen(colors.scaleSeparator, style.separatorWidth, Qt::SolidLine, Qt::FlatCap,
             Qt::RoundJoin));
    painter.drawPath(sampledArc(scale.center, scale.radius - style.separatorInset,
                                scale.startRadians, scale.endRadians));
    painter.setPen(
        QPen(colors.scaleCalibration, style.calibrationWidth, Qt::SolidLine, Qt::FlatCap,
             Qt::RoundJoin));
    painter.drawPath(sampledArc(scale.center, scale.radius - style.calibrationInset,
                                scale.startRadians, scale.endRadians));
    painter.setPen(QPen(colors.scaleInner, style.innerWidth, Qt::SolidLine, Qt::FlatCap,
                        Qt::RoundJoin));
    painter.drawPath(sampledArc(scale.center, scale.radius - style.innerInset, scale.startRadians,
                                scale.endRadians));

    const QPen minorPen(colors.minorTick, style.minorTickWidth, Qt::SolidLine, Qt::SquareCap);
    for (int i = 0; i + 1 < scale.anglesRadians.size(); ++i) {
        const double firstValue = scale.values[i];
        const double secondValue = scale.values[i + 1];
        for (int subdivision = 1; subdivision < scale.minorSubdivisions; ++subdivision) {
            const double fraction = static_cast<double>(subdivision) / scale.minorSubdivisions;
            const double value = std::lerp(firstValue, secondValue, fraction);
            const double angle = CrossNeedleMeterGeometry::angleForValue(scale, value);
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
    painter.setPen(QPen(colors.majorTick, style.majorTickWidth, Qt::SolidLine, Qt::SquareCap));
    for (int i = 0; i < scale.anglesRadians.size(); ++i) {
        const double angle = CrossNeedleMeterGeometry::printedAngleForIndex(scale, i);
        const QPointF radial(std::cos(angle), std::sin(angle));
        const QPointF point = scale.center + radial * scale.radius;
        painter.drawLine(point - radial * 20.0, point + radial * 14.0);
        if (!scale.labels[i].isEmpty()) {
            painter.save();
            painter.setPen(colors.text);
            drawCenteredText(painter, point + radial * scale.labelOffset, scale.labels[i]);
            painter.restore();
        }
    }

    QFont titleFont = font();
    titleFont.setPixelSize(m_geometry.typography.sideTitlePixels);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(colors.text);
    drawRotatedText(painter, title.center, title.text, title.rotationDegrees);
}

void CrossNeedleMeterWidget::drawSwrGuides(QPainter &painter) const {
    const AnalogMeterFaceThemeCatalog::Palette &colors =
        m_faceThemes.palette(analogFaceTheme());
    painter.setPen(QPen(colors.swrGuide, m_geometry.swrStyle.guideWidth, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    // Trim the leading (lower) end of every contour to sit a small gap above
    // the mask, like the real meter face: clip guide drawing to the region
    // above the mask boundary raised by maskGap. Labels below are unaffected.
    painter.save();
    const QVector<QPointF> &maskEdge = m_geometry.mask.boundary;
    const double maskGap = m_geometry.swrStyle.maskGap;
    if (maskGap > 0.0 && maskEdge.size() >= 2) {
        QPainterPath above;
        above.moveTo(0.0, 0.0);
        above.lineTo(m_geometry.canvasWidth, 0.0);
        above.lineTo(m_geometry.canvasWidth, maskEdge.last().y() - maskGap);
        for (int i = maskEdge.size() - 1; i >= 0; --i) {
            above.lineTo(maskEdge[i].x(), maskEdge[i].y() - maskGap);
        }
        above.lineTo(0.0, maskEdge.first().y() - maskGap);
        above.closeSubpath();
        painter.setClipPath(above, Qt::IntersectClip);
    }
    for (const CrossNeedleMeterGeometry::SwrGuide &guide : m_geometry.swrGuides) {
        painter.drawPath(m_geometry.swrGuidePath(guide));
    }
    painter.restore();

    QFont labelFont = font();
    labelFont.setPixelSize(m_geometry.typography.swrNumberPixels);
    labelFont.setBold(true);
    painter.setFont(labelFont);
    const QFontMetricsF metrics(labelFont);
    for (const CrossNeedleMeterGeometry::SwrGuide &guide : m_geometry.swrGuides) {
        if (guide.displayLabel.isEmpty()) {
            continue;
        }
        const QString display = guide.displayLabel == QStringLiteral("infinity")
                                    ? QString::fromUtf8("\xe2\x88\x9e")
                                    : guide.displayLabel;
        const QRectF textRect = metrics.boundingRect(display).adjusted(-3.0, -2.0, 3.0, 2.0);
        const QPointF labelCenter =
            m_geometry.swrGuideLabelCenter(guide, labelFont);
        QRectF background = textRect;
        background.moveCenter(labelCenter);
        const CrossNeedleMeterGeometry::Frame &frame = m_geometry.frame;
        const QRectF face(frame.faceInset, frame.faceInset,
                          m_geometry.canvasWidth - 2.0 * frame.faceInset,
                          m_geometry.canvasHeight - 2.0 * frame.faceInset);
        painter.save();
        painter.setClipRect(background);
        drawFaceBackground(painter, face);
        painter.restore();
        painter.setPen(colors.swrLabel);
        drawCenteredText(painter, labelCenter, display);
    }
}

void CrossNeedleMeterWidget::drawFaceBackground(QPainter &painter, const QRectF &face) const {
    m_faceThemes.drawBackground(painter, face, analogFaceTheme());
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

    const AnalogMeterFaceThemeCatalog::Palette &colors =
        m_faceThemes.palette(analogFaceTheme());
    if (m_rangeLegendVisible) {
        QFont rangeFont = font();
        rangeFont.setPixelSize(m_geometry.typography.rangePixels);
        rangeFont.setBold(true);
        painter.setFont(rangeFont);
        painter.setPen(colors.secondaryText);
        drawCenteredMultilineText(painter, m_geometry.rangeLabelCenter,
                                  m_geometry.rangeLabel);
    }

    QFont unitFont = font();
    unitFont.setPixelSize(m_geometry.typography.unitPixels);
    unitFont.setBold(true);
    painter.setFont(unitFont);
    painter.setPen(colors.text);
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
    const AnalogMeterFaceThemeCatalog::Palette &colors =
        m_faceThemes.palette(analogFaceTheme());
    const CrossNeedleMeterGeometry::Frame &frame = m_geometry.frame;
    const QRectF face(frame.faceInset, frame.faceInset,
                      m_geometry.canvasWidth - 2.0 * frame.faceInset,
                      m_geometry.canvasHeight - 2.0 * frame.faceInset);
    const QVector<QPointF> boundary = m_faceThemes.lowerMaskBoundary(face);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.maskFill);
    painter.drawPath(m_faceThemes.lowerMaskPath(face));

    painter.setPen(QPen(colors.maskEdge, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolyline(QPolygonF(boundary));

    QFont labelFont = font();
    labelFont.setPixelSize(m_geometry.typography.maskLabelPixels);
    labelFont.setBold(true);
    painter.setFont(labelFont);
    painter.setPen(colors.maskText);
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

    const AnalogMeterFaceThemeCatalog::Palette &colors =
        m_faceThemes.palette(analogFaceTheme());
    const CrossNeedleMeterGeometry::NeedleStyle &style = m_geometry.needleStyle;
    const QColor shadow = colors.needleShadow;
    const QColor line = colors.needle;
    const QColor edge = colors.needleEdge;
    const QColor highlight = colors.needleHighlight;
    const auto drawMovement = [&painter, &style, &colors, shadow, line, edge, highlight](
                                  const QPointF &pivot, const QPointF &tip) {
        // Build physical depth from code-drawn material layers. Only the soft
        // shadows and sub-pixel edge reflections are displaced; the body is
        // always painted on the calibrated pivot-to-tip ray. The cast shadows
        // remain continuous over the printed scale, as they would on a
        // physical meter face beneath an elevated needle.
        painter.setPen(QPen(colors.needleSoftShadow, style.softShadowWidth, Qt::SolidLine,
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
    setProperty("rangeLegendVisible", m_rangeLegendVisible);
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
