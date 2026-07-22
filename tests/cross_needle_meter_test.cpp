// Deterministic construction and mechanics regression test for the PWR
// applet's cross-needle Forward/Reflected power and SWR face.

#include "TestSettingsProfile.h"
#include "gui/CrossNeedleMeterGeometry.h"
#include "gui/CrossNeedleMeterWidget.h"

#include <QAccessible>
#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontMetricsF>
#include <QImage>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char *name, bool ok, const std::string &detail = {}) {
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

bool near(double actual, double expected, double tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

int darkestPixelSumNear(const QImage &image, const QPointF &point, int radius) {
    int darkest = 255 * 3;
    const int centerX = qRound(point.x());
    const int centerY = qRound(point.y());
    for (int y = centerY - radius; y <= centerY + radius; ++y) {
        for (int x = centerX - radius; x <= centerX + radius; ++x) {
            if (!image.rect().contains(x, y)) {
                continue;
            }
            const QColor color = image.pixelColor(x, y);
            darkest = std::min(darkest, color.red() + color.green() + color.blue());
        }
    }
    return darkest;
}

QImage renderWidget(QWidget &widget) {
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

void processEventsFor(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

int differingPixels(const QImage &first, const QImage &second) {
    if (first.size() != second.size()) {
        return -1;
    }
    int count = 0;
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            if (first.pixel(x, y) != second.pixel(x, y)) {
                ++count;
            }
        }
    }
    return count;
}

int differingPixels(const QImage &first, const QImage &second, const QRect &area) {
    if (first.size() != second.size()) {
        return -1;
    }
    const QRect clipped = area.intersected(first.rect());
    int count = 0;
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            if (first.pixel(x, y) != second.pixel(x, y)) {
                ++count;
            }
        }
    }
    return count;
}

double meanLuminance(const QImage &image, const QRect &area) {
    const QRect clipped = area.intersected(image.rect());
    if (clipped.isEmpty()) {
        return 0.0;
    }
    double total = 0.0;
    int count = 0;
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            total += 0.2126 * color.red() + 0.7152 * color.green() + 0.0722 * color.blue();
            ++count;
        }
    }
    return count > 0 ? total / count : 0.0;
}

double distanceToSegment(const QPointF &point, const QPointF &start, const QPointF &end) {
    const QPointF segment = end - start;
    const double lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared <= 0.0) {
        return std::hypot(point.x() - start.x(), point.y() - start.y());
    }
    const double fraction = std::clamp(
        QPointF::dotProduct(point - start, segment) / lengthSquared, 0.0, 1.0);
    const QPointF nearest = start + segment * fraction;
    return std::hypot(point.x() - nearest.x(), point.y() - nearest.y());
}

double distanceToPathOrUpperContinuation(const QPointF &point, const QPainterPath &path) {
    if (path.isEmpty()) {
        return std::numeric_limits<double>::infinity();
    }
    double distance = std::numeric_limits<double>::infinity();
    for (int element = 1; element < path.elementCount(); ++element) {
        distance = std::min(distance,
                            distanceToSegment(point, path.elementAt(element - 1),
                                              path.elementAt(element)));
    }
    if (path.elementCount() >= 2) {
        const QPointF end = path.currentPosition();
        QPointF direction = end - QPointF(path.elementAt(path.elementCount() - 2));
        const double length = std::hypot(direction.x(), direction.y());
        if (length > 1e-12) {
            direction /= length;
            const double projection = std::max(0.0, QPointF::dotProduct(point - end, direction));
            const QPointF nearest = end + direction * projection;
            distance = std::min(distance,
                                std::hypot(point.x() - nearest.x(), point.y() - nearest.y()));
        }
    }
    return distance;
}

QPointF pointOnScale(const CrossNeedleMeterGeometry::Scale &scale, double angle) {
    return scale.center + QPointF(std::cos(angle), std::sin(angle)) * scale.radius;
}

double targetPowerRatio(const CrossNeedleMeterGeometry::SwrGuide &guide) {
    if (std::isinf(guide.swr)) {
        return 1.0;
    }
    const double reflectionCoefficient = (guide.swr - 1.0) / (guide.swr + 1.0);
    return reflectionCoefficient * reflectionCoefficient;
}

QPointF pointOnPathAtY(const QPainterPath &path, double y) {
    if (path.isEmpty()) {
        return {};
    }
    QPointF closest = path.elementAt(0);
    double closestDistance = std::abs(closest.y() - y);
    for (int index = 1; index < path.elementCount(); ++index) {
        const QPointF first = path.elementAt(index - 1);
        const QPointF second = path.elementAt(index);
        const double firstOffset = first.y() - y;
        const double secondOffset = second.y() - y;
        if (firstOffset * secondOffset <= 0.0 && first.y() != second.y()) {
            const double fraction = (y - first.y()) / (second.y() - first.y());
            return first + (second - first) * fraction;
        }
        if (std::abs(secondOffset) < closestDistance) {
            closest = second;
            closestDistance = std::abs(secondOffset);
        }
    }
    return closest;
}

double maskBoundaryYAtX(const CrossNeedleMeterGeometry &geometry, double x) {
    const QVector<QPointF> &boundary = geometry.mask.boundary;
    if (boundary.isEmpty()) {
        return geometry.mask.bottomY;
    }
    if (x <= boundary.first().x()) {
        return boundary.first().y();
    }
    for (int index = 1; index < boundary.size(); ++index) {
        const QPointF first = boundary[index - 1];
        const QPointF second = boundary[index];
        if (x <= second.x()) {
            const double span = second.x() - first.x();
            const double fraction = span > 0.0 ? (x - first.x()) / span : 0.0;
            return first.y() + (second.y() - first.y()) * fraction;
        }
    }
    return boundary.last().y();
}

void testResourceAndConstruction() {
    QString error;
    const CrossNeedleMeterGeometry geometry = CrossNeedleMeterGeometry::loadResource(&error);
    report("semantic cross-needle JSON resource validates", geometry.isValid(),
           error.toStdString());
    report("compiled degraded fallback remains mechanically valid",
           CrossNeedleMeterGeometry::fallback().isValid());
    if (!geometry.isValid()) {
        return;
    }

    CrossNeedleMeterGeometry impossibleLabelGeometry = geometry;
    impossibleLabelGeometry.swrGuides = {geometry.swrGuides.first()};
    impossibleLabelGeometry.typography.swrNumberPixels = 2000;
    QString impossibleLabelError;
    report("resource validation rejects an unsatisfiable SWR label layout",
           !impossibleLabelGeometry.isValid(&impossibleLabelError) &&
               impossibleLabelError.contains(QStringLiteral("label")),
           impossibleLabelError.toStdString());

    const auto rejectsMask = [&geometry](CrossNeedleMeterGeometry candidate,
                                         const char *description) {
        QString maskError;
        report(description,
               !candidate.isValid(&maskError) &&
                   maskError.contains(QStringLiteral("lower mask")),
               maskError.toStdString());
    };
    CrossNeedleMeterGeometry unsortedMask = geometry;
    std::swap(unsortedMask.mask.boundary[1], unsortedMask.mask.boundary[2]);
    rejectsMask(unsortedMask, "resource validation rejects an unsorted lower mask");
    CrossNeedleMeterGeometry duplicateMaskX = geometry;
    duplicateMaskX.mask.boundary[1].setX(duplicateMaskX.mask.boundary[0].x());
    rejectsMask(duplicateMaskX, "resource validation rejects duplicate mask x coordinates");
    CrossNeedleMeterGeometry outOfBoundsMask = geometry;
    outOfBoundsMask.mask.boundary[0].setX(-1.0);
    rejectsMask(outOfBoundsMask, "resource validation rejects an out-of-bounds lower mask");
    CrossNeedleMeterGeometry asymmetricMask = geometry;
    asymmetricMask.mask.boundary[0].setY(
        asymmetricMask.mask.boundary[0].y() - 1.0);
    rejectsMask(asymmetricMask, "resource validation rejects an asymmetric lower mask");
    CrossNeedleMeterGeometry nonFiniteMask = geometry;
    nonFiniteMask.mask.boundary[0].setY(
        std::numeric_limits<double>::infinity());
    rejectsMask(nonFiniteMask, "resource validation rejects a non-finite lower mask");

    report("semantic geometry format is versioned", geometry.formatVersion == 6);
    report("angled-rest concave response design revision is retained",
           geometry.designVersion == 19);
    report("face material keeps editable deterministic gradient layers",
           geometry.faceGradient.middleStop > 0.0 && geometry.faceGradient.middleStop < 1.0 &&
               geometry.faceGradient.glowRadius > 0.0 &&
               geometry.faceGradient.vignetteRadius > 0.0 &&
               geometry.faceGradient.vignetteClearStop > 0.0);
    report("dark-room uplight keeps editable deterministic lamp layers",
           geometry.uplightGradient.haloRadius > geometry.uplightGradient.hotspotRadius &&
               geometry.uplightGradient.hotspotRadius > geometry.uplightGradient.bloomRadius &&
               geometry.uplightGradient.haloCenter.x() == geometry.canvasWidth / 2.0 &&
               geometry.uplightGradient.hotspotCenter.x() == geometry.canvasWidth / 2.0 &&
               geometry.uplightGradient.bloomCenter.x() == geometry.canvasWidth / 2.0 &&
               geometry.uplightGradient.hotspotCenter.y() >
                   geometry.uplightGradient.haloCenter.y() &&
               geometry.uplightGradient.bloomInner.alpha() >
                   geometry.uplightGradient.haloInner.alpha() &&
               geometry.uplightGradient.haloShoulderStop >
                   geometry.uplightGradient.haloMiddleStop &&
               geometry.uplightGradient.paperGrainOpacity >= 0.09 &&
               geometry.uplightGradient.scaleSeparator.alpha() < 255 &&
               geometry.uplightGradient.scaleSeparator.red() < 160);
    report("graphite dark keeps editable deterministic material layers",
           geometry.darkTheme.ambientRadius > geometry.darkTheme.glowRadius &&
               geometry.darkTheme.ambientCenter.x() == geometry.canvasWidth / 2.0 &&
               geometry.darkTheme.glowCenter.x() == geometry.canvasWidth / 2.0 &&
               geometry.darkTheme.paperGrainOpacity >= 0.20 &&
               geometry.darkTheme.text.red() + geometry.darkTheme.text.green() +
                       geometry.darkTheme.text.blue() >
                   geometry.darkTheme.middle.red() + geometry.darkTheme.middle.green() +
                           geometry.darkTheme.middle.blue() +
                       400 &&
               geometry.darkTheme.scaleInner.blue() > geometry.darkTheme.scaleInner.red());
    report("needles keep an editable restrained physical material stack",
           geometry.needleStyle.edgeWidth < geometry.needleStyle.lineWidth &&
               geometry.needleStyle.highlightWidth < geometry.needleStyle.lineWidth &&
               geometry.needleStyle.edgeOffset > geometry.needleStyle.highlightOffset &&
               geometry.needleStyle.softShadowWidth > geometry.needleStyle.shadowWidth &&
               geometry.needleStyle.softShadowOffset.x() >
                   geometry.needleStyle.shadowOffset.x() &&
               geometry.needleStyle.softShadowOffset.y() >
                   geometry.needleStyle.shadowOffset.y() &&
               geometry.needleStyle.softShadow.alpha() < geometry.needleStyle.shadow.alpha() &&
               geometry.darkTheme.needleHighlight.lightness() >
                   geometry.darkTheme.needle.lightness() &&
               geometry.darkTheme.needleEdge.lightness() <
                   geometry.darkTheme.needle.lightness());
    report("power scales retain editable code-drawn vintage ink layers",
           geometry.scaleStyle.ribbonWidth >= 24.0 && geometry.scaleStyle.outerWidth >= 7.0 &&
               geometry.scaleStyle.separatorWidth >= 3.0 &&
               geometry.scaleStyle.calibrationWidth >= 3.0 &&
               geometry.scaleStyle.innerWidth >= 4.0 &&
               geometry.scaleStyle.majorTickWidth > geometry.scaleStyle.minorTickWidth &&
               geometry.scaleStyle.calibration.red() > geometry.scaleStyle.calibration.green() &&
               geometry.scaleStyle.inner.blue() > geometry.scaleStyle.inner.red());
    report("two concealed movement pivots are level",
           near(geometry.forwardScale.center.y(), geometry.reflectedScale.center.y(), 0.001));
    report("concealed movement pivots mirror about face center",
           near(geometry.forwardScale.center.x() + geometry.reflectedScale.center.x(),
                geometry.canvasWidth, 0.001));
    report("both pivots remain below the visible face",
           geometry.forwardScale.center.y() > geometry.canvasHeight &&
               geometry.reflectedScale.center.y() > geometry.canvasHeight);

    const QPointF forwardStart =
        pointOnScale(geometry.forwardScale, geometry.forwardScale.startRadians);
    const QPointF reflectedStart =
        pointOnScale(geometry.reflectedScale, geometry.reflectedScale.startRadians);
    report("left and right graph starts are level",
           near(forwardStart.y(), reflectedStart.y(), 0.01));
    report("left and right graph starts are horizontally mirrored",
           near(forwardStart.x() + reflectedStart.x(), geometry.canvasWidth, 0.01));

    const QPointF forwardEnd =
        pointOnScale(geometry.forwardScale, geometry.forwardScale.endRadians);
    const QPointF reflectedEnd =
        pointOnScale(geometry.reflectedScale, geometry.reflectedScale.endRadians);
    report("top graph endpoints are visually symmetric",
           near(forwardEnd.x() + reflectedEnd.x(), geometry.canvasWidth, 8.0) &&
               near(forwardEnd.y(), reflectedEnd.y(), 8.0));

    report("forward graph retains 16 calibrated major ticks",
           geometry.forwardScale.values.size() == 16);
    report("reflected graph retains 16 calibrated major ticks",
           geometry.reflectedScale.values.size() == 16);
    double maximumRuntimeTickErrorPixels = 0.0;
    double maximumReferenceFitErrorPixels = 0.0;
    bool referenceFitsWithinDeclaredBudget = true;
    for (int index = 0; index < geometry.forwardScale.values.size(); ++index) {
        const double angle =
            geometry.forwardAngle(geometry.forwardScale.values[index], 1.0);
        maximumRuntimeTickErrorPixels =
            std::max(maximumRuntimeTickErrorPixels,
                     std::abs(angle - geometry.forwardScale.anglesRadians[index]) *
                         geometry.forwardScale.radius);
        if (index > 0) {
            const double referenceError =
                std::abs(angle - geometry.forwardScale.referenceAnglesRadians[index]) *
                geometry.forwardScale.radius;
            maximumReferenceFitErrorPixels =
                std::max(maximumReferenceFitErrorPixels, referenceError);
            referenceFitsWithinDeclaredBudget =
                referenceFitsWithinDeclaredBudget &&
                referenceError <= geometry.forwardScale.maximumReferenceErrorPixels;
        }
    }
    for (int index = 0; index < geometry.reflectedScale.values.size(); ++index) {
        const double angle =
            geometry.reflectedAngle(geometry.reflectedScale.values[index], 1.0);
        maximumRuntimeTickErrorPixels =
            std::max(maximumRuntimeTickErrorPixels,
                     std::abs(angle - geometry.reflectedScale.anglesRadians[index]) *
                         geometry.reflectedScale.radius);
        if (index > 0) {
            const double referenceError =
                std::abs(angle - geometry.reflectedScale.referenceAnglesRadians[index]) *
                geometry.reflectedScale.radius;
            maximumReferenceFitErrorPixels =
                std::max(maximumReferenceFitErrorPixels, referenceError);
            referenceFitsWithinDeclaredBudget =
                referenceFitsWithinDeclaredBudget &&
                referenceError <= geometry.reflectedScale.maximumReferenceErrorPixels;
        }
    }
    const auto responseIsMonotone = [](const CrossNeedleMeterGeometry::Scale &scale) {
        for (int index = 1; index < scale.responseCoefficients.size(); ++index) {
            if (scale.responseCoefficients[index] + 1e-12 <
                scale.responseCoefficients[index - 1]) {
                return false;
            }
        }
        return true;
    };
    const auto responseIsConcave = [](const CrossNeedleMeterGeometry::Scale &scale) {
        for (int index = 0; index + 2 < scale.responseCoefficients.size(); ++index) {
            if (scale.responseCoefficients[index + 2] -
                    2.0 * scale.responseCoefficients[index + 1] +
                    scale.responseCoefficients[index] >
                1e-12) {
                return false;
            }
        }
        return true;
    };
    report("both movements use a smooth monotonic concave response",
           geometry.forwardScale.responseModel == QStringLiteral("concave_bernstein_v1") &&
               geometry.reflectedScale.responseModel == QStringLiteral("concave_bernstein_v1") &&
               geometry.forwardScale.responseCoefficients.size() == 6 &&
               geometry.reflectedScale.responseCoefficients.size() == 6 &&
               responseIsMonotone(geometry.forwardScale) &&
               responseIsMonotone(geometry.reflectedScale) &&
               responseIsConcave(geometry.forwardScale) &&
               responseIsConcave(geometry.reflectedScale));
    report("calibrated positive-power ticks use the exact live movement response",
           maximumRuntimeTickErrorPixels <= 1e-8,
           "max_error_px=" + std::to_string(maximumRuntimeTickErrorPixels));
    report("needles park on their printed zero mark (angled rest on the dial)",
           near(CrossNeedleMeterGeometry::printedAngleForIndex(geometry.forwardScale, 0),
                geometry.forwardScale.anglesRadians[0], 1e-12) &&
               near(geometry.forwardScale.anglesRadians[0],
                    geometry.forwardScale.responseStartRadians, 1e-12) &&
               near(CrossNeedleMeterGeometry::printedAngleForIndex(geometry.reflectedScale, 0),
                    geometry.reflectedScale.anglesRadians[0], 1e-12) &&
               near(geometry.reflectedScale.anglesRadians[0],
                    geometry.reflectedScale.responseStartRadians, 1e-12));
    report("constrained movement fits retain the photographed scale observations",
           referenceFitsWithinDeclaredBudget,
           "max_reference_error_px=" +
               std::to_string(maximumReferenceFitErrorPixels));

    double maximumSecondDerivativeJump = 0.0;
    const auto measureSecondDerivativeJumps =
        [&maximumSecondDerivativeJump](const CrossNeedleMeterGeometry::Scale &scale,
                                       const auto &angleAt) {
            const double step = scale.values.last() * 1e-4;
            for (int index = 1; index + 1 < scale.values.size(); ++index) {
                const double value = scale.values[index];
                const double atKnot = angleAt(value);
                const double leftSecond =
                    (atKnot - 2.0 * angleAt(value - step) +
                     angleAt(value - 2.0 * step)) /
                    (step * step);
                const double rightSecond =
                    (angleAt(value + 2.0 * step) -
                     2.0 * angleAt(value + step) + atKnot) /
                    (step * step);
                maximumSecondDerivativeJump =
                    std::max(maximumSecondDerivativeJump,
                             std::abs(rightSecond - leftSecond));
            }
        };
    measureSecondDerivativeJumps(
        geometry.forwardScale,
        [&geometry](double value) { return geometry.forwardAngle(value, 1.0); });
    measureSecondDerivativeJumps(
        geometry.reflectedScale,
        [&geometry](double value) { return geometry.reflectedAngle(value, 1.0); });
    report("movement second derivatives stay continuous across calibration knots",
           maximumSecondDerivativeJump <= 0.01,
           "max_jump=" + std::to_string(maximumSecondDerivativeJump));
    report("forward graph retains five-way minor subdivision",
           geometry.forwardScale.minorSubdivisions == 5);
    report("reflected graph retains two-way minor subdivision",
           geometry.reflectedScale.minorSubdivisions == 2);
    report("reflected graph is continuously painted behind Forward graph",
           geometry.scaleOverlap.reflectedBehindForward);
    report("reflected underpass mask remains deliberately narrow",
           geometry.scaleOverlap.reflectedGapHalfSpanRadians > 0.0 &&
               geometry.scaleOverlap.reflectedGapHalfSpanRadians <= 0.015);
    report("side labels use readable mirrored rotations",
           near(geometry.forwardTitle.rotationDegrees, -geometry.reflectedTitle.rotationDegrees,
                0.001) &&
               geometry.forwardTitle.rotationDegrees < 0.0 &&
               geometry.reflectedTitle.rotationDegrees > 0.0);
    report("power-unit labels remain mirrored outside the top arcs",
           near(geometry.forwardUnitCenter.x() + geometry.reflectedUnitCenter.x(),
                geometry.canvasWidth, 0.001) &&
               near(geometry.forwardUnitCenter.y(), geometry.reflectedUnitCenter.y(), 0.001) &&
               geometry.forwardUnitCenter.x() < 400.0 && geometry.reflectedUnitCenter.x() > 1100.0);
    report("power numbers clear the graph ticks", geometry.forwardScale.labelOffset >= 50.0 &&
                                                      geometry.reflectedScale.labelOffset >= 50.0);
    report("side titles move outward symmetrically with scale numbers",
           near(geometry.forwardTitle.center.x() + geometry.reflectedTitle.center.x(),
                geometry.canvasWidth, 0.001) &&
               geometry.forwardTitle.center.x() <= 145.0 &&
               geometry.reflectedTitle.center.x() >= 1355.0);
    report("range legend uses four short top-right lines",
           geometry.rangeLabel.count(QLatin1Char('\n')) == 3 &&
               geometry.rangeLabelCenter.x() >= 1300.0 && geometry.rangeLabelCenter.y() <= 125.0);
    report("face typography retains enlarged readable design sizes",
           geometry.typography.scaleNumberPixels >= 42 &&
               geometry.typography.sideTitlePixels >= 50 &&
               geometry.typography.swrNumberPixels >= 36 && geometry.typography.rangePixels >= 26 &&
               geometry.typography.unitPixels >= 40 && geometry.typography.maskLabelPixels >= 46);
    const QStringList expectedGuideOrder = {
        QStringLiteral("infinity"), QStringLiteral("8"),   QStringLiteral("4"),
        QStringLiteral("3"),        QStringLiteral("2.5"), QStringLiteral("2"),
        QStringLiteral("1.7"),      QStringLiteral("1.5"), QStringLiteral("1.4"),
        QStringLiteral("1.3"),      QStringLiteral("1.2"), QStringLiteral("1.1")};
    report("SWR fan retains all 12 semantic constant-ratio guides",
           geometry.swrGuides.size() == expectedGuideOrder.size());
    report("SWR construction exposes editable clearance, gap, and sampling",
           geometry.swrStyle.graphClearance > 0.0 &&
               geometry.swrStyle.graphClearance <
                   std::min(geometry.forwardScale.radius, geometry.reflectedScale.radius) &&
               geometry.swrStyle.maskGap >= 0.0 && geometry.swrStyle.curveSamples >= 96);

    bool guideOrderCorrect = geometry.swrGuides.size() == expectedGuideOrder.size();
    bool everyLabelVisible = true;
    bool sampledPaths = true;
    bool contoursShareConvergence = true;
    QPointF convergencePoint;
    double maximumConvergenceSpread = 0.0;
    QVector<double> visibleLowerAngleDeg(geometry.swrGuides.size(), -1.0);
    QVector<int> visibleSampleCount(geometry.swrGuides.size(), 0);
    bool mechanicallyCalibrated = true;
    bool labelsNearestOwnGuide = true;
    QStringList misassociatedLabels;
    bool orderedAboveMask = true;
    double maximumEnvelopeGapError = 0.0;
    double maximumRatioError = 0.0;
    double maximumSegmentRatioError = 0.0;
    double maximumPathApproximationError = 0.0;
    double maximumTurnRadians = 0.0;
    double maximumCurvatureStep = 0.0;
    int totalCurvatureReversals = 0;
    int maximumGuideCurvatureReversals = 0;
    double maximumCurveBow = 0.0;
    double minimumLabelSpacing = std::numeric_limits<double>::infinity();
    double minimumOwnGuideMargin = std::numeric_limits<double>::infinity();
    double previousConstructionX = -std::numeric_limits<double>::infinity();
    QVector<QPointF> labelCenters;
    QVector<QRectF> labelRects;
    QFont swrLabelFont = QApplication::font();
    swrLabelFont.setPixelSize(geometry.typography.swrNumberPixels);
    swrLabelFont.setBold(true);
    const QFontMetricsF swrLabelMetrics(swrLabelFont);

    for (int guideIndex = 0; guideIndex < geometry.swrGuides.size(); ++guideIndex) {
        const CrossNeedleMeterGeometry::SwrGuide &guide = geometry.swrGuides[guideIndex];
        guideOrderCorrect = guideOrderCorrect && guide.label == expectedGuideOrder[guideIndex];
        everyLabelVisible = everyLabelVisible && !guide.displayLabel.isEmpty();

        const QPainterPath path = geometry.swrGuidePath(guide);
        sampledPaths = sampledPaths &&
                       path.elementCount() == geometry.swrStyle.curveSamples + 1;
        if (path.isEmpty()) {
            mechanicallyCalibrated = false;
            continue;
        }
        // Angled-rest model: at zero power both needles park on their printed
        // zeros, so every contour begins at the SAME hidden crossing just below
        // the mask (the physical rest crossing the whole family fans from).
        const QPointF first = path.elementAt(0);
        if (guideIndex == 0) {
            convergencePoint = first;
        } else {
            maximumConvergenceSpread =
                std::max(maximumConvergenceSpread,
                         std::hypot(first.x() - convergencePoint.x(),
                                    first.y() - convergencePoint.y()));
        }

        // First visible segment above the mask: its angle from the horizontal
        // baseline characterizes how shallow the low-SWR lines emerge. Also
        // count visible samples so 1.1/1.2 are confirmed to reach the face.
        QPointF firstVisible;
        bool haveFirstVisible = false;
        for (int element = 0; element < path.elementCount(); ++element) {
            const QPointF point = path.elementAt(element);
            if (point.y() < maskBoundaryYAtX(geometry, point.x())) {
                ++visibleSampleCount[guideIndex];
                if (!haveFirstVisible) {
                    firstVisible = point;
                    haveFirstVisible = true;
                }
            }
        }
        if (haveFirstVisible) {
            const QPointF top = path.currentPosition();
            const double dx = std::abs(top.x() - firstVisible.x());
            const double dy = std::abs(top.y() - firstVisible.y());
            visibleLowerAngleDeg[guideIndex] =
                std::atan2(dy, std::max(dx, 1e-9)) * 180.0 / M_PI;
        }

        const double targetRatio = targetPowerRatio(guide);
        // The contour is an exact constant-ratio locus only within the readable
        // range; past full scale it is extended as face art to the common
        // termination boundary (see docs D1). Verify the ratio only where both
        // movements read strictly inside their calibrated maxima.
        const double forwardReadMax = geometry.forwardScale.values.last() - 1e-3;
        const double reflectedReadMax = geometry.reflectedScale.values.last() - 1e-3;
        const auto readable = [&](const QPointF &p) {
            return p.x() > 1e-6 && p.x() < forwardReadMax && p.y() < reflectedReadMax;
        };
        QVector<double> visibleCurvatures;
        for (int element = 1; element < path.elementCount(); ++element) {
            const QPointF point = path.elementAt(element);
            const QPointF powers = geometry.powerReadingsAtIntersection(point);
            if (readable(powers)) {
                const double ratio = powers.y() / powers.x();
                const double relativeError = std::abs(ratio - targetRatio) / targetRatio;
                maximumRatioError = std::max(maximumRatioError, relativeError);
                mechanicallyCalibrated = mechanicallyCalibrated && relativeError <= 1e-6;
            }

            const QPointF previous = path.elementAt(element - 1);
            const QPointF midpoint = (previous + point) * 0.5;
            const bool midpointVisible =
                midpoint.y() < maskBoundaryYAtX(geometry, midpoint.x());
            const QPointF midpointPowers = geometry.powerReadingsAtIntersection(midpoint);
            const QPointF previousPowers = geometry.powerReadingsAtIntersection(previous);
            // Only verify the exact-locus approximation on segments wholly
            // inside the readable range; the extended art tip is exempt.
            if (midpointVisible && readable(midpointPowers) && readable(powers) &&
                readable(previousPowers)) {
                const double midpointRatio = midpointPowers.y() / midpointPowers.x();
                maximumSegmentRatioError =
                    std::max(maximumSegmentRatioError,
                             std::abs(midpointRatio - targetRatio) / targetRatio);
                const double previousVoltage =
                    std::sqrt(previousPowers.x() / geometry.forwardScale.values.last());
                const double currentVoltage =
                    std::sqrt(powers.x() / geometry.forwardScale.values.last());
                const double middleVoltage = (previousVoltage + currentVoltage) * 0.5;
                const double exactForward =
                    geometry.forwardScale.values.last() * middleVoltage * middleVoltage;
                const QPointF exactMidpoint =
                    geometry.needleIntersection(exactForward, exactForward * targetRatio, 1.0);
                maximumPathApproximationError =
                    std::max(maximumPathApproximationError,
                             std::hypot(exactMidpoint.x() - midpoint.x(),
                                        exactMidpoint.y() - midpoint.y()));
            }

            if (midpointVisible && element >= 2) {
                const QPointF before = path.elementAt(element - 2);
                const QPointF firstVector = previous - before;
                const QPointF secondVector = point - previous;
                const double firstLength = std::hypot(firstVector.x(), firstVector.y());
                const double secondLength = std::hypot(secondVector.x(), secondVector.y());
                if (firstLength > 1e-9 && secondLength > 1e-9) {
                    const double cosine = std::clamp(
                        QPointF::dotProduct(firstVector, secondVector) /
                            (firstLength * secondLength),
                        -1.0, 1.0);
                    maximumTurnRadians =
                        std::max(maximumTurnRadians, std::acos(cosine));
                    const double signedCross = firstVector.x() * secondVector.y() -
                                               firstVector.y() * secondVector.x();
                    const double signedTurn = std::atan2(
                        signedCross, QPointF::dotProduct(firstVector, secondVector));
                    visibleCurvatures.append(signedTurn / ((firstLength + secondLength) * 0.5));
                }
            }
            maximumCurveBow =
                std::max(maximumCurveBow,
                         distanceToSegment(point, first, path.currentPosition()));
        }

        QVector<double> smoothedCurvatures;
        smoothedCurvatures.reserve(visibleCurvatures.size());
        for (int index = 0; index < visibleCurvatures.size(); ++index) {
            double sum = 0.0;
            int count = 0;
            for (int offset = -2; offset <= 2; ++offset) {
                const int sample = std::clamp(index + offset, 0,
                                              static_cast<int>(visibleCurvatures.size()) - 1);
                sum += visibleCurvatures[sample];
                ++count;
            }
            smoothedCurvatures.append(sum / count);
        }
        double maximumGuideCurvature = 0.0;
        for (double curvature : smoothedCurvatures) {
            maximumGuideCurvature = std::max(maximumGuideCurvature, std::abs(curvature));
        }
        const double curvatureThreshold = maximumGuideCurvature * 0.025;
        int previousCurvatureSign = 0;
        int guideCurvatureReversals = 0;
        for (int index = 0; index < smoothedCurvatures.size(); ++index) {
            const double curvature = smoothedCurvatures[index];
            if (index > 0) {
                maximumCurvatureStep =
                    std::max(maximumCurvatureStep,
                             std::abs(curvature - smoothedCurvatures[index - 1]));
            }
            if (std::abs(curvature) <= curvatureThreshold) {
                continue;
            }
            const int sign = curvature > 0.0 ? 1 : -1;
            if (previousCurvatureSign != 0 && sign != previousCurvatureSign) {
                ++guideCurvatureReversals;
            }
            previousCurvatureSign = sign;
        }
        totalCurvatureReversals += guideCurvatureReversals;
        maximumGuideCurvatureReversals =
            std::max(maximumGuideCurvatureReversals, guideCurvatureReversals);

        // Design 19: EVERY contour terminates on one common boundary a fixed
        // graph_clearance short of whichever power arc is nearer. Measure the
        // endpoint's gap to the nearer arc directly (geometric distance, which
        // is not fooled by the past-full-scale power clamp) and require all 12
        // to sit at that clearance. This also guards swrGuidePath's fallback:
        // if a contour ever failed to reach the envelope it would terminate
        // elsewhere and this deviation would flag it.
        const QPointF endpoint = geometry.swrGuideUpperEndpoint(guide);
        const double forwardGap = geometry.forwardScale.radius -
                                  std::hypot(endpoint.x() - geometry.forwardScale.center.x(),
                                             endpoint.y() - geometry.forwardScale.center.y());
        const double reflectedGap = geometry.reflectedScale.radius -
                                    std::hypot(endpoint.x() - geometry.reflectedScale.center.x(),
                                               endpoint.y() - geometry.reflectedScale.center.y());
        const double graphGap = std::min(forwardGap, reflectedGap);
        maximumEnvelopeGapError =
            std::max(maximumEnvelopeGapError,
                     std::abs(graphGap - geometry.swrStyle.graphClearance));

        const QPointF constructionPoint = pointOnPathAtY(path, 900.0);
        orderedAboveMask = orderedAboveMask && constructionPoint.x() > previousConstructionX;
        previousConstructionX = constructionPoint.x();

        const QPointF labelCenter =
            geometry.swrGuideLabelCenter(guide, swrLabelFont);
        labelCenters.append(labelCenter);
        const QString display = guide.displayLabel == QStringLiteral("infinity")
                                    ? QString::fromUtf8("\xe2\x88\x9e")
                                    : guide.displayLabel;
        QRectF labelRect =
            swrLabelMetrics.boundingRect(display).adjusted(-3.0, -2.0, 3.0, 2.0);
        labelRect.moveCenter(labelCenter);
        labelRects.append(labelRect);
        const double ownDistance = distanceToPathOrUpperContinuation(labelCenter, path);
        double nearestOtherDistance = std::numeric_limits<double>::infinity();
        QString nearestLabel = guide.label;
        for (int candidateIndex = 0; candidateIndex < geometry.swrGuides.size();
             ++candidateIndex) {
            if (candidateIndex == guideIndex) {
                continue;
            }
            const double candidateDistance = distanceToPathOrUpperContinuation(
                labelCenter, geometry.swrGuidePath(geometry.swrGuides[candidateIndex]));
            if (candidateDistance < nearestOtherDistance) {
                nearestOtherDistance = candidateDistance;
                if (candidateDistance < ownDistance) {
                    nearestLabel = geometry.swrGuides[candidateIndex].label;
                }
            }
        }
        labelsNearestOwnGuide = labelsNearestOwnGuide && nearestLabel == guide.label;
        if (nearestLabel != guide.label) {
            misassociatedLabels.append(guide.label + QStringLiteral("/") + nearestLabel);
        }
        minimumOwnGuideMargin =
            std::min(minimumOwnGuideMargin, nearestOtherDistance - ownDistance);
    }
    bool labelBoxesSeparated = true;
    QStringList overlappingLabelBoxes;
    for (int first = 0; first < labelCenters.size(); ++first) {
        for (int second = first + 1; second < labelCenters.size(); ++second) {
            minimumLabelSpacing =
                std::min(minimumLabelSpacing,
                         std::hypot(labelCenters[first].x() - labelCenters[second].x(),
                                    labelCenters[first].y() - labelCenters[second].y()));
            if (labelRects[first].intersects(labelRects[second])) {
                labelBoxesSeparated = false;
                overlappingLabelBoxes.append(
                    geometry.swrGuides[first].label + QLatin1Char('/') +
                    geometry.swrGuides[second].label);
            }
        }
    }

    bool labelBoxesClearOtherGuides = true;
    QStringList labelsCrossingOtherGuides;
    bool labelBoxesClearMask = true;
    QStringList labelsCrossingMask;
    for (int labelIndex = 0; labelIndex < labelRects.size(); ++labelIndex) {
        const QRectF &labelRect = labelRects[labelIndex];
        const bool labelClearsMask =
            labelRect.bottom() + 3.0 < maskBoundaryYAtX(geometry, labelRect.left()) &&
            labelRect.bottom() + 3.0 < maskBoundaryYAtX(geometry, labelRect.center().x()) &&
            labelRect.bottom() + 3.0 < maskBoundaryYAtX(geometry, labelRect.right());
        labelBoxesClearMask = labelBoxesClearMask && labelClearsMask;
        if (!labelClearsMask) {
            labelsCrossingMask.append(geometry.swrGuides[labelIndex].label);
        }
        for (int guideIndex = 0; guideIndex < geometry.swrGuides.size(); ++guideIndex) {
            if (labelIndex == guideIndex) {
                continue;
            }
            if (geometry.swrGuidePath(geometry.swrGuides[guideIndex])
                    .intersects(labelRects[labelIndex])) {
                labelBoxesClearOtherGuides = false;
                labelsCrossingOtherGuides.append(
                    geometry.swrGuides[labelIndex].label + QStringLiteral(" box/") +
                    geometry.swrGuides[guideIndex].label + QStringLiteral(" guide"));
            }
        }
    }

    QFont scaleFont = QApplication::font();
    scaleFont.setPixelSize(geometry.typography.scaleNumberPixels);
    scaleFont.setWeight(QFont::Medium);
    const QFontMetricsF scaleMetrics(scaleFont);
    bool swrLabelsClearReflectedNumbers = true;
    for (int index = 0; index < geometry.reflectedScale.labels.size(); ++index) {
        const QString &display = geometry.reflectedScale.labels[index];
        if (display.isEmpty()) {
            continue;
        }
        const double angle =
            CrossNeedleMeterGeometry::printedAngleForIndex(geometry.reflectedScale, index);
        const QPointF radial(std::cos(angle), std::sin(angle));
        const QPointF center = pointOnScale(geometry.reflectedScale, angle) +
                               radial * geometry.reflectedScale.labelOffset;
        QRectF reflectedRect = scaleMetrics.boundingRect(display);
        reflectedRect.moveCenter(center);
        for (const QRectF &swrRect : labelRects) {
            swrLabelsClearReflectedNumbers =
                swrLabelsClearReflectedNumbers && !swrRect.intersects(reflectedRect);
        }
    }

    report("SWR guide order and every printed label are retained",
           guideOrderCorrect && everyLabelVisible);
    report("all contours share one convergence concealed below the lower mask",
           contoursShareConvergence && maximumConvergenceSpread <= 1e-6 &&
               convergencePoint.y() >= geometry.mask.bottomY,
           "spread_px=" + std::to_string(maximumConvergenceSpread) + " convergence=(" +
               std::to_string(convergencePoint.x()) + "," +
               std::to_string(convergencePoint.y()) + ")");
    // Guide order: infinity, 8, 4, 3, 2.5, 2, 1.7, 1.5, 1.4, 1.3, 1.2, 1.1.
    report("low-SWR contours (1.1, 1.2) reach the visible face",
           visibleSampleCount[10] >= 5 && visibleSampleCount[11] >= 5,
           "visible_samples_1.2=" + std::to_string(visibleSampleCount[10]) +
               " visible_samples_1.1=" + std::to_string(visibleSampleCount[11]));
    bool lowSwrShallowSteepening =
        visibleLowerAngleDeg[11] > 0.0 && visibleLowerAngleDeg[11] < 35.0 &&
        visibleLowerAngleDeg[10] < 45.0;
    for (int i = 11; i > 7; --i) {  // 1.1 < 1.2 < 1.3 < 1.4 < 1.5
        lowSwrShallowSteepening = lowSwrShallowSteepening &&
                                  visibleLowerAngleDeg[i] > 0.0 &&
                                  visibleLowerAngleDeg[i - 1] > visibleLowerAngleDeg[i];
    }
    report("low-SWR lines emerge shallow and steepen toward higher SWR",
           lowSwrShallowSteepening,
           "angle_1.1=" + std::to_string(visibleLowerAngleDeg[11]) +
               " angle_1.2=" + std::to_string(visibleLowerAngleDeg[10]) +
               " angle_1.5=" + std::to_string(visibleLowerAngleDeg[7]));
    report("every sampled contour point preserves its printed power ratio",
           sampledPaths && mechanicallyCalibrated && maximumRatioError <= 1e-6,
           "max_relative_error=" + std::to_string(maximumRatioError));
    report("drawn line segments preserve ratio between exact samples",
           maximumSegmentRatioError <= 0.001,
           "max_relative_error=" + std::to_string(maximumSegmentRatioError));
    report("sampled path stays within the exact constant-ratio locus",
           maximumPathApproximationError <= 0.03,
           "max_error_px=" + std::to_string(maximumPathApproximationError));
    report("mechanical contours are smooth rather than traced squiggles",
           maximumTurnRadians <= 0.025 && maximumCurveBow >= 1.0,
           "max_turn_rad=" + std::to_string(maximumTurnRadians) +
               " max_bow_px=" + std::to_string(maximumCurveBow));
    report("visible contour curvature has no tick-by-tick oscillation",
           maximumGuideCurvatureReversals <= 1,
           "total_reversals=" + std::to_string(totalCurvatureReversals) +
               " max_per_guide=" + std::to_string(maximumGuideCurvatureReversals) +
               " max_curvature_step=" + std::to_string(maximumCurvatureStep));
    report("every contour terminates on the common graph-clearance envelope",
           maximumEnvelopeGapError <= 1.0,
           "max_envelope_gap_error_px=" + std::to_string(maximumEnvelopeGapError));
    report("SWR contours remain ordered above the lower mask", orderedAboveMask);
    report("every SWR number is associated with its own contour",
           labelsNearestOwnGuide && minimumOwnGuideMargin >= 1.0,
           "minimum_margin_px=" + std::to_string(minimumOwnGuideMargin) +
               " mismatches=" + misassociatedLabels.join(QStringLiteral(", ")).toStdString());
    report("SWR number anchors retain readable separation",
           minimumLabelSpacing >= 38.0,
           "minimum_spacing_px=" + std::to_string(minimumLabelSpacing));
    report("SWR number boxes do not overlap one another", labelBoxesSeparated,
           overlappingLabelBoxes.join(QStringLiteral(", ")).toStdString());
    report("SWR number backgrounds cannot erase a neighboring contour",
           labelBoxesClearOtherGuides,
           labelsCrossingOtherGuides.join(QStringLiteral(", ")).toStdString());
    report("SWR number boxes remain above the lower mask", labelBoxesClearMask,
           labelsCrossingMask.join(QStringLiteral(", ")).toStdString());
    report("SWR numbers remain clear of Reflected-scale numbers",
           swrLabelsClearReflectedNumbers);
    report("SWR contour ink survives applet-size downsampling",
           geometry.swrStyle.guideWidth >= 3.0);

    QFont wideSwrLabelFont = swrLabelFont;
    wideSwrLabelFont.setStretch(175);
    const QFontMetricsF wideSwrLabelMetrics(wideSwrLabelFont);
    QVector<QRectF> wideLabelRects;
    bool fontChangeRepositionedLabels = false;
    bool wideLabelBoxesSeparated = true;
    bool wideLabelBoxesClearMask = true;
    for (int index = 0; index < geometry.swrGuides.size(); ++index) {
        const CrossNeedleMeterGeometry::SwrGuide &guide = geometry.swrGuides[index];
        const QPointF center =
            geometry.swrGuideLabelCenter(guide, wideSwrLabelFont);
        fontChangeRepositionedLabels =
            fontChangeRepositionedLabels ||
            std::hypot(center.x() - labelCenters[index].x(),
                       center.y() - labelCenters[index].y()) > 0.01;
        const QString display = guide.displayLabel == QStringLiteral("infinity")
                                    ? QString::fromUtf8("\xe2\x88\x9e")
                                    : guide.displayLabel;
        QRectF box =
            wideSwrLabelMetrics.boundingRect(display).adjusted(-3.0, -2.0, 3.0, 2.0);
        box.moveCenter(center);
        wideLabelBoxesClearMask =
            wideLabelBoxesClearMask &&
            box.bottom() + 3.0 < maskBoundaryYAtX(geometry, box.left()) &&
            box.bottom() + 3.0 < maskBoundaryYAtX(geometry, box.center().x()) &&
            box.bottom() + 3.0 < maskBoundaryYAtX(geometry, box.right());
        wideLabelRects.append(box);
    }
    for (int first = 0; first < wideLabelRects.size(); ++first) {
        for (int second = first + 1; second < wideLabelRects.size(); ++second) {
            wideLabelBoxesSeparated =
                wideLabelBoxesSeparated &&
                !wideLabelRects[first].intersects(wideLabelRects[second]);
        }
    }
    report("SWR label placement cache is keyed by the rendered font",
           fontChangeRepositionedLabels);
    report("wider rendered-font SWR labels remain collision-free",
           wideLabelBoxesSeparated && wideLabelBoxesClearMask);

    bool maskSymmetric = true;
    for (int i = 0; i < geometry.mask.boundary.size(); ++i) {
        const QPointF left = geometry.mask.boundary[i];
        const QPointF right = geometry.mask.boundary[geometry.mask.boundary.size() - 1 - i];
        maskSymmetric = maskSymmetric && near(left.x() + right.x(), geometry.canvasWidth, 0.01) &&
                        near(left.y(), right.y(), 0.01);
    }
    report("lower opaque mask is bilaterally symmetric", maskSymmetric);
}

void testMeterMathAndActiveProof() {
    const CrossNeedleMeterGeometry geometry = CrossNeedleMeterGeometry::loadResource();
    const double reflected = CrossNeedleMeterGeometry::reflectedPowerWatts(100.0, 1.5);
    report("100 W at SWR 1.5 derives 4 W reflected", near(reflected, 4.0, 1e-9));
    report("4 W reflected from 100 W reconstructs SWR 1.5",
           near(CrossNeedleMeterGeometry::swrFromPowers(100.0, 4.0), 1.5, 1e-9));
    report("zero forward power reports neutral SWR",
           CrossNeedleMeterGeometry::swrFromPowers(0.0, 0.0) == 1.0);
    report("full reflection reports infinite SWR",
           std::isinf(CrossNeedleMeterGeometry::swrFromPowers(100.0, 100.0)));

    report("20 W hardware selects x1 range",
           CrossNeedleMeterGeometry::rangeMultiplierFor(20, false) == 1.0);
    report("100 W radio selects x10 range",
           CrossNeedleMeterGeometry::rangeMultiplierFor(100, false) == 10.0);
    report("amplifier output selects x100 range",
           CrossNeedleMeterGeometry::rangeMultiplierFor(100, true) == 100.0);

    const QPointF documentedIntersection = geometry.needleIntersection(10.0, 0.4, 1.0);
    const QPointF multipliedIntersection = geometry.needleIntersection(100.0, 4.0, 10.0);
    const double documentedRangeError =
        std::hypot(documentedIntersection.x() - multipliedIntersection.x(),
                   documentedIntersection.y() - multipliedIntersection.y());
    double documentedGuideDistance = 0.0;
    const QString documentedGuide =
        geometry.nearestGuideLabel(documentedIntersection, &documentedGuideDistance);
    report("10 W / 0.4 W and 100 W / 4 W produce the same needle intersection",
           documentedRangeError <= 1e-6,
           "error_px=" + std::to_string(documentedRangeError));
    report("documented 10 W / 0.4 W calibration lands on SWR 1.5",
           documentedGuide == QStringLiteral("1.5") && documentedGuideDistance <= 0.1,
           "guide=" + documentedGuide.toStdString() +
               " error_px=" + std::to_string(documentedGuideDistance));

    const QPointF intersection = geometry.needleIntersection(
        geometry.validation.activeForwardWatts, geometry.validation.activeReflectedWatts,
        geometry.validation.rangeMultiplier);
    const double intersectionError =
        std::hypot(intersection.x() - geometry.validation.intersection.x(),
                   intersection.y() - geometry.validation.intersection.y());
    report("active needle intersection matches approved proof", intersectionError < 0.01,
           "actual=(" + std::to_string(intersection.x()) + "," +
               std::to_string(intersection.y()) + ") error_px=" +
               std::to_string(intersectionError));

    double guideDistance = 0.0;
    const QString nearestGuide = geometry.nearestGuideLabel(intersection, &guideDistance);
    report("active intersection lands on printed 1.5 guide",
           nearestGuide == geometry.validation.guide &&
               guideDistance <= geometry.validation.maximumGuideError,
           "guide=" + nearestGuide.toStdString() + " error_px=" + std::to_string(guideDistance));
    report("active proof lies on the generated constant-ratio contour",
           guideDistance <= 0.1,
           "error_px=" + std::to_string(guideDistance));

    bool everyGuideSelectsItself = true;
    bool everyGuideReconstructsSWR = true;
    bool rangeInvariant = true;
    double maximumGuideDistance = 0.0;
    double maximumSWRerror = 0.0;
    double maximumRangeError = 0.0;
    constexpr double kFractions[] = {0.45, 0.70, 0.90};
    for (const CrossNeedleMeterGeometry::SwrGuide &guide : geometry.swrGuides) {
        const double ratio = targetPowerRatio(guide);
        // Sample within the READABLE range (both movements <= full scale) AND
        // within the drawn contour (crossing still inside the termination
        // envelope) — a contour may hit the envelope before full scale.
        const double readableForward =
            std::min(geometry.forwardScale.values.last(),
                     geometry.reflectedScale.values.last() / ratio);
        const double clearance = geometry.swrStyle.graphClearance;
        const auto insideEnvelope = [&](double f) {
            const QPointF p = geometry.needleIntersection(f, f * ratio, 1.0);
            return std::hypot(p.x() - geometry.forwardScale.center.x(),
                              p.y() - geometry.forwardScale.center.y()) <
                       geometry.forwardScale.radius - clearance &&
                   std::hypot(p.x() - geometry.reflectedScale.center.x(),
                              p.y() - geometry.reflectedScale.center.y()) <
                       geometry.reflectedScale.radius - clearance;
        };
        double contourForward = readableForward;
        if (!insideEnvelope(readableForward)) {
            double lower = 0.0;
            double upper = readableForward;
            for (int k = 0; k < 48; ++k) {
                const double middle = (lower + upper) * 0.5;
                if (insideEnvelope(middle)) {
                    lower = middle;
                } else {
                    upper = middle;
                }
            }
            contourForward = lower;
        }
        for (const double fraction : kFractions) {
            const double forwardPower = contourForward * fraction;
            const double reflectedPower = forwardPower * ratio;
            const QPointF point =
                geometry.needleIntersection(forwardPower, reflectedPower, 1.0);
            double distance = 0.0;
            const QString selectedGuide = geometry.nearestGuideLabel(point, &distance);
            everyGuideSelectsItself = everyGuideSelectsItself &&
                                      selectedGuide == guide.label && distance <= 0.1;
            maximumGuideDistance = std::max(maximumGuideDistance, distance);

            const double reconstructed =
                CrossNeedleMeterGeometry::swrFromPowers(forwardPower, reflectedPower);
            if (std::isinf(guide.swr)) {
                everyGuideReconstructsSWR = everyGuideReconstructsSWR &&
                                            std::isinf(reconstructed);
            } else {
                const double error = std::abs(reconstructed - guide.swr);
                maximumSWRerror = std::max(maximumSWRerror, error);
                everyGuideReconstructsSWR = everyGuideReconstructsSWR && error <= 1e-9;
            }

            for (const double multiplier : {10.0, 100.0}) {
                const QPointF multiplied = geometry.needleIntersection(
                    forwardPower * multiplier, reflectedPower * multiplier, multiplier);
                const double error =
                    std::hypot(multiplied.x() - point.x(), multiplied.y() - point.y());
                maximumRangeError = std::max(maximumRangeError, error);
                rangeInvariant = rangeInvariant && error <= 1e-6;
            }
        }
    }
    report("every printed SWR guide selects itself at multiple powers",
           everyGuideSelectsItself,
           "maximum_distance_px=" + std::to_string(maximumGuideDistance));
    report("every printed guide reconstructs its declared SWR",
           everyGuideReconstructsSWR,
           "maximum_swr_error=" + std::to_string(maximumSWRerror));
    report("SWR contours are invariant across x1, x10, and x100 ranges",
           rangeInvariant,
           "maximum_position_error_px=" + std::to_string(maximumRangeError));
}

void testWidgetStateAndRender() {
    CrossNeedleMeterWidget meter;
    meter.resize(600, 400);
    report("widget preserves approved 3:2 aspect hint", meter.heightForWidth(600) == 400);
    report("cross-needle face defaults to dark-room uplight lighting",
           meter.faceTheme() == CrossNeedleMeterWidget::FaceTheme::DarkRoomUplight &&
               meter.faceThemeId() == QStringLiteral("dark-room-uplight"));
    meter.setFaceTheme(CrossNeedleMeterWidget::FaceTheme::ClassicWarm);
    report("RX starts with both movements parked",
           !meter.isTransmitting() && meter.forwardWatts() == 0.0 &&
               meter.reflectedWatts() == 0.0 && meter.swr() == 1.0);
    report("Range legend is visible by default", meter.rangeLegendVisible());
    const QImage rangeVisible = renderWidget(meter);
    const double multiplierBeforeHide = meter.rangeMultiplier();
    const double forwardAngleBeforeHide = meter.geometry().forwardAngle(0.0, 1.0);
    const double reflectedAngleBeforeHide = meter.geometry().reflectedAngle(0.0, 1.0);
    meter.setRangeLegendVisible(false);
    const QImage rangeHidden = renderWidget(meter);
    const QRect rangeLegendRegion(480, 16, 90, 70);
    const QRect unaffectedControlRegion(20, 180, 90, 100);
    report("Show Range hides the printed legend",
           !meter.rangeLegendVisible() &&
               differingPixels(rangeVisible, rangeHidden, rangeLegendRegion) > 100);
    report("hiding Range does not change unrelated face ink",
           differingPixels(rangeVisible, rangeHidden, unaffectedControlRegion) == 0);
    report("hiding Range does not change scale calibration",
           near(meter.rangeMultiplier(), multiplierBeforeHide, 1e-12) &&
               near(meter.geometry().forwardAngle(0.0, 1.0),
                    forwardAngleBeforeHide, 1e-12) &&
               near(meter.geometry().reflectedAngle(0.0, 1.0),
                    reflectedAngleBeforeHide, 1e-12));
    meter.setRangeLegendVisible(true);
    const QImage rangeRestored = renderWidget(meter);
    report("re-enabling Range restores the deterministic face exactly",
           differingPixels(rangeVisible, rangeRestored) == 0);
    const QImage swrInkBaseline = renderWidget(meter);

    meter.setPowerScale(200, false);
    meter.setTransmitting(true);
    meter.setTxMeters(100.0f, 1.5f);
    report("widget retains calculated reflected-power fallback",
           meter.isTransmitting() && near(meter.forwardWatts(), 100.0, 1e-6) &&
               near(meter.reflectedWatts(), 4.0, 1e-6) && near(meter.swr(), 1.5, 1e-6) &&
               !meter.reflectedPowerMeasured() && meter.rangeMultiplier() == 10.0);

    meter.setTxPowers(100.0f, 4.0f);
    report("widget consumes independent forward and reflected power",
           near(meter.forwardWatts(), 100.0, 1e-6) && near(meter.reflectedWatts(), 4.0, 1e-6) &&
               near(meter.swr(), 1.5, 1e-6) && meter.reflectedPowerMeasured());

    const QImage animationStart = renderWidget(meter);
    processEventsFor(120);
    const QImage animationSettled = renderWidget(meter);
    const int animationPixels = differingPixels(animationStart, animationSettled);
    report("live smoother timer visibly moves both needle rendering", animationPixels > 250,
           "changed_pixels=" + std::to_string(animationPixels));

    processEventsFor(1800);
    const QImage needleMaterialProof = renderWidget(meter);
    const CrossNeedleMeterGeometry &needleGeometry = meter.geometry();
    const QPointF forwardTip = needleGeometry.forwardTip(100.0, 10.0);
    const QPointF reflectedTip = needleGeometry.reflectedTip(4.0, 10.0);
    const QPointF parkedForwardTip = needleGeometry.forwardTip(0.0, 10.0);
    const QPointF parkedReflectedTip = needleGeometry.reflectedTip(0.0, 10.0);
    const CrossNeedleMeterGeometry::NeedleStyle &needleStyle = needleGeometry.needleStyle;
    const double designScale = static_cast<double>(needleMaterialProof.width()) /
                               needleGeometry.canvasWidth;
    int shadowCrossingPixels = 0;
    int shadowCrossingPixelsChanged = 0;
    int unaffectedSwrPixels = 0;
    int unaffectedSwrPixelsChanged = 0;
    const auto movementDistance = [](const QPointF &point, const QPointF &pivot,
                                     const QPointF &tip, const QPointF &offset) {
        return distanceToSegment(point, pivot + offset, tip + offset);
    };
    const auto nearNeedleBody = [&movementDistance, &needleGeometry, &needleStyle](
                                    const QPointF &point, const QPointF &forward,
                                    const QPointF &reflected) {
        const double clearance = needleStyle.lineWidth / 2.0 +
                                 needleGeometry.swrStyle.guideWidth / 2.0 + 1.0;
        return movementDistance(point, needleGeometry.forwardScale.center, forward, {}) <=
                   clearance ||
               movementDistance(point, needleGeometry.reflectedScale.center, reflected, {}) <=
                   clearance;
    };
    const auto nearNeedleShadow = [&movementDistance, &needleGeometry, &needleStyle](
                                      const QPointF &point, const QPointF &forward,
                                      const QPointF &reflected) {
        const double guideRadius = needleGeometry.swrStyle.guideWidth / 2.0;
        const double contactRadius = needleStyle.shadowWidth / 2.0 + guideRadius;
        const double softRadius = needleStyle.softShadowWidth / 2.0 + guideRadius;
        return movementDistance(point, needleGeometry.forwardScale.center, forward,
                                needleStyle.shadowOffset) <= contactRadius ||
               movementDistance(point, needleGeometry.reflectedScale.center, reflected,
                                needleStyle.shadowOffset) <= contactRadius ||
               movementDistance(point, needleGeometry.forwardScale.center, forward,
                                needleStyle.softShadowOffset) <= softRadius ||
               movementDistance(point, needleGeometry.reflectedScale.center, reflected,
                                needleStyle.softShadowOffset) <= softRadius;
    };
    for (const CrossNeedleMeterGeometry::SwrGuide &guide : needleGeometry.swrGuides) {
        const QPainterPath path = needleGeometry.swrGuidePath(guide);
        constexpr int kGuideSamples = 240;
        for (int i = 0; i <= kGuideSamples; ++i) {
            const QPointF designPoint =
                path.pointAtPercent(static_cast<double>(i) / kGuideSamples);
            if (designPoint.y() >= 850.0) {
                continue;
            }
            const QPoint pixel = (designPoint * designScale).toPoint();
            if (!needleMaterialProof.rect().contains(pixel)) {
                continue;
            }
            const bool changed =
                needleMaterialProof.pixel(pixel) != swrInkBaseline.pixel(pixel);
            const bool activeBody = nearNeedleBody(designPoint, forwardTip, reflectedTip);
            const bool activeShadow = nearNeedleShadow(designPoint, forwardTip, reflectedTip);
            const bool parkedBody =
                nearNeedleBody(designPoint, parkedForwardTip, parkedReflectedTip);
            const bool parkedShadow =
                nearNeedleShadow(designPoint, parkedForwardTip, parkedReflectedTip);
            if (activeShadow && !activeBody && !parkedBody && !parkedShadow) {
                ++shadowCrossingPixels;
                if (changed) {
                    ++shadowCrossingPixelsChanged;
                }
            } else if (!activeBody && !activeShadow && !parkedBody && !parkedShadow) {
                ++unaffectedSwrPixels;
                if (changed) {
                    ++unaffectedSwrPixelsChanged;
                }
            }
        }
    }
    report("needle cast shadows remain continuous across printed SWR guides",
           shadowCrossingPixels > 10 &&
               shadowCrossingPixelsChanged * 2 >= shadowCrossingPixels,
           "crossings=" + std::to_string(shadowCrossingPixels) +
               " changed=" + std::to_string(shadowCrossingPixelsChanged));
    report("needle depth leaves distant SWR guide printing stable",
           unaffectedSwrPixels > 1000 &&
               unaffectedSwrPixelsChanged * 100 < unaffectedSwrPixels,
           "compared=" + std::to_string(unaffectedSwrPixels) +
               " changed=" + std::to_string(unaffectedSwrPixelsChanged));

    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(&meter);
    report("custom-painted meter exposes an accessible Indicator role",
           accessible && accessible->role() == QAccessible::Indicator);
    const QString accessibleValue = accessible ? accessible->text(QAccessible::Value) : QString();
    report("accessible value describes both movements and SWR",
           accessibleValue.contains(QStringLiteral("forward 100.0 watts")) &&
               accessibleValue.contains(QStringLiteral("reflected 4.0 watts")) &&
               accessibleValue.contains(QStringLiteral("SWR 1.50")));

    const QString proofPath = qEnvironmentVariable("AETHER_CROSS_NEEDLE_PROOF");
    if (!proofPath.isEmpty()) {
        processEventsFor(1800);
        const QImage activeProof = renderWidget(meter);
        report("optional active-state render proof is writable", activeProof.save(proofPath),
               proofPath.toStdString());
    }
    const QString uplightProofPath = qEnvironmentVariable("AETHER_CROSS_NEEDLE_UPLIGHT_PROOF");
    if (!uplightProofPath.isEmpty()) {
        meter.setFaceTheme(CrossNeedleMeterWidget::FaceTheme::DarkRoomUplight);
        const QImage uplightProof = renderWidget(meter);
        report("optional dark-room uplight render proof is writable",
               uplightProof.save(uplightProofPath), uplightProofPath.toStdString());
        meter.setFaceTheme(CrossNeedleMeterWidget::FaceTheme::ClassicWarm);
    }
    const QString darkProofPath = qEnvironmentVariable("AETHER_CROSS_NEEDLE_DARK_PROOF");
    if (!darkProofPath.isEmpty()) {
        meter.setFaceTheme(CrossNeedleMeterWidget::FaceTheme::GraphiteDark);
        const QImage darkProof = renderWidget(meter);
        report("optional graphite-dark render proof is writable", darkProof.save(darkProofPath),
               darkProofPath.toStdString());
        meter.setFaceTheme(CrossNeedleMeterWidget::FaceTheme::ClassicWarm);
    }

    meter.setTransmitting(false);
    meter.setTxPowers(80.0f, 5.0f); // Deliberately late UDP packet after the RX edge.
    report("late power packet is cached while RX remains authoritative",
           !meter.isTransmitting() && near(meter.forwardWatts(), 80.0, 1e-6) &&
               near(meter.reflectedWatts(), 5.0, 1e-6) &&
               meter.accessibleValueText().contains(QStringLiteral("Receive")) &&
               !meter.property("effectiveActive").toBool());

    processEventsFor(2200);
    const double parkedForward = meter.property("displayedForwardWatts").toDouble();
    const double parkedReflected = meter.property("displayedReflectedWatts").toDouble();
    report("late power packet cannot move parked needles",
           near(parkedForward, 0.0, 0.05) && near(parkedReflected, 0.0, 0.05),
           "forward=" + std::to_string(parkedForward) +
               " reflected=" + std::to_string(parkedReflected));

    meter.setTransmitting(true);
    processEventsFor(1800);
    report("next TX edge reuses the cached meter sample",
           meter.property("effectiveActive").toBool() &&
               near(meter.property("displayedForwardWatts").toDouble(), 80.0, 0.1) &&
               near(meter.property("displayedReflectedWatts").toDouble(), 5.0, 0.1));
    meter.setTransmitting(false);
    processEventsFor(1800);

    processEventsFor(1800);
    const QImage rendered = renderWidget(meter);
    const QColor face = rendered.pixelColor(300, 120);
    const QColor faceCenter = rendered.pixelColor(300, 160);
    const QColor faceUpperLeft = rendered.pixelColor(22, 22);
    const QColor faceUpperRight = rendered.pixelColor(578, 22);
    const QColor mask = rendered.pixelColor(100, 370);
    report("deterministic render keeps warm illuminated face",
           face.red() > 210 && face.green() > 200 && face.blue() > 180, face.name().toStdString());
    report("old-instrument gradient keeps center brighter than edges",
           faceCenter.red() + faceCenter.green() + faceCenter.blue() >
               faceUpperLeft.red() + faceUpperLeft.green() + faceUpperLeft.blue() + 20);
    report("old-instrument edge vignette remains symmetric",
           std::abs(faceUpperLeft.red() - faceUpperRight.red()) <= 3 &&
               std::abs(faceUpperLeft.green() - faceUpperRight.green()) <= 3 &&
               std::abs(faceUpperLeft.blue() - faceUpperRight.blue()) <= 3);
    report("deterministic render keeps dark symmetric lower mask",
           mask.red() < 90 && mask.green() < 90 && mask.blue() < 100, mask.name().toStdString());

    meter.setFaceTheme(CrossNeedleMeterWidget::FaceTheme::DarkRoomUplight);
    const QImage uplightRendered = renderWidget(meter);
    const int themePixels = differingPixels(rendered, uplightRendered);
    const double uplightUpper = meanLuminance(uplightRendered, QRect(250, 24, 100, 34));
    const double uplightMiddle = meanLuminance(uplightRendered, QRect(250, 175, 100, 34));
    const double uplightLower = meanLuminance(uplightRendered, QRect(250, 302, 100, 34));
    const double uplightCore = meanLuminance(uplightRendered, QRect(275, 322, 50, 20));
    const double classicUpper = meanLuminance(rendered, QRect(250, 24, 100, 34));
    report("dark-room uplight is a distinct deterministic face theme", themePixels > 100000,
           "changed_pixels=" + std::to_string(themePixels));
    report("dark-room lamp exposure rises from top toward bottom center",
           uplightLower > uplightUpper + 55.0,
           "top=" + std::to_string(uplightUpper) + " bottom=" + std::to_string(uplightLower));
    report("dark-room broad halo illuminates the middle of the card",
           uplightMiddle > uplightUpper + 35.0 && uplightLower > uplightMiddle + 45.0,
           "top=" + std::to_string(uplightUpper) + " middle=" +
               std::to_string(uplightMiddle) + " bottom=" + std::to_string(uplightLower));
    report("dark-room theme lowers ambient exposure at the top", classicUpper > uplightUpper + 40.0,
           "classic=" + std::to_string(classicUpper) + " uplight=" + std::to_string(uplightUpper));
    report("dark-room lamp has a concentrated paper-diffusion bloom",
           uplightCore > uplightLower + 2.0,
           "core=" + std::to_string(uplightCore) +
               " lower=" + std::to_string(uplightLower));
    report("dark-room selection is reflected by the widget API",
           meter.faceTheme() == CrossNeedleMeterWidget::FaceTheme::DarkRoomUplight &&
               meter.faceThemeId() == QStringLiteral("dark-room-uplight"));
    const CrossNeedleMeterGeometry::Scale &forwardScale = meter.geometry().forwardScale;
    const double graphProbeAngle =
        forwardScale.anglesRadians[4] +
        0.33 * (forwardScale.anglesRadians[5] - forwardScale.anglesRadians[4]);
    const QPointF graphProbeDesign =
        forwardScale.center + QPointF(std::cos(graphProbeAngle), std::sin(graphProbeAngle)) *
                                  (forwardScale.radius - 20.0);
    const QPoint graphProbe = (graphProbeDesign * 0.4).toPoint();
    const QColor classicGraphBackdrop = rendered.pixelColor(graphProbe);
    const QColor uplightGraphBackdrop = uplightRendered.pixelColor(graphProbe);
    report("uplight gradient remains visible between graph ink rules",
           classicGraphBackdrop.red() + classicGraphBackdrop.green() + classicGraphBackdrop.blue() >
               uplightGraphBackdrop.red() + uplightGraphBackdrop.green() +
                   uplightGraphBackdrop.blue() + 120,
           "classic=" + classicGraphBackdrop.name().toStdString() +
               " uplight=" + uplightGraphBackdrop.name().toStdString());
    const QPointF graphProbeRadial(std::cos(graphProbeAngle), std::sin(graphProbeAngle));
    const QPoint separatorProbe =
        ((forwardScale.center +
          graphProbeRadial * (forwardScale.radius - meter.geometry().scaleStyle.separatorInset)) *
         0.4)
            .toPoint();
    const QColor classicSeparator = rendered.pixelColor(separatorProbe);
    const QColor uplightSeparator = uplightRendered.pixelColor(separatorProbe);
    report("uplight graph separator no longer creates a white backing",
           classicSeparator.red() + classicSeparator.green() + classicSeparator.blue() >
               uplightSeparator.red() + uplightSeparator.green() + uplightSeparator.blue() + 120,
           "classic=" + classicSeparator.name().toStdString() +
               " uplight=" + uplightSeparator.name().toStdString());

    meter.setFaceTheme(CrossNeedleMeterWidget::FaceTheme::GraphiteDark);
    const QImage darkRendered = renderWidget(meter);
    const int darkThemePixels = differingPixels(rendered, darkRendered);
    const double darkUpper = meanLuminance(darkRendered, QRect(250, 24, 100, 34));
    const double darkMiddle = meanLuminance(darkRendered, QRect(250, 175, 100, 34));
    const double darkLower = meanLuminance(darkRendered, QRect(250, 302, 100, 34));
    const QColor darkGraphBackdrop = darkRendered.pixelColor(graphProbe);
    report("graphite dark is a distinct deterministic face theme", darkThemePixels > 100000,
           "changed_pixels=" + std::to_string(darkThemePixels));
    report("graphite dark keeps the complete card at dark-mode exposure",
           darkUpper < 75.0 && darkMiddle < 90.0 && darkLower < 115.0,
           "top=" + std::to_string(darkUpper) + " middle=" + std::to_string(darkMiddle) +
               " bottom=" + std::to_string(darkLower));
    report("graphite dark retains a restrained lower-center amber lift",
           darkLower > darkUpper + 8.0,
           "top=" + std::to_string(darkUpper) + " bottom=" + std::to_string(darkLower));
    report("graphite dark remains visible behind both graph bands",
           darkGraphBackdrop.red() + darkGraphBackdrop.green() + darkGraphBackdrop.blue() < 240,
           darkGraphBackdrop.name().toStdString());
    report("graphite dark selection is reflected by the widget API",
           meter.faceTheme() == CrossNeedleMeterWidget::FaceTheme::GraphiteDark &&
               meter.faceThemeId() == QStringLiteral("graphite-dark"));
    meter.setFaceTheme(CrossNeedleMeterWidget::FaceTheme::ClassicWarm);

    const CrossNeedleMeterGeometry &geometry = meter.geometry();
    const QPointF reflectedGapProbe = pointOnScale(geometry.reflectedScale, -1.10) * 0.4;
    const int reflectedInk = darkestPixelSumNear(rendered, reflectedGapProbe, 3);
    report("reflected graph remains inked through former upper gap", reflectedInk < 500,
           "darkest_rgb_sum=" + std::to_string(reflectedInk));
}

} // namespace

int main(int argc, char **argv) {
    TestSettingsProfile settingsProfile(QStringLiteral("aether-cross-needle-meter-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "[FAIL] could not create temporary settings profile\n");
        return 1;
    }
    qputenv("AETHER_AUTOMATION", "1");
    QApplication application(argc, argv);
    std::printf("Cross-needle meter construction test harness\n\n");

    testResourceAndConstruction();
    testMeterMathAndActiveProof();
    testWidgetStateAndRender();

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed."
                                        : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
