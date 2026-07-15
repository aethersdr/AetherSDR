// Deterministic construction and mechanics regression test for the PWR
// applet's cross-needle Forward/Reflected power and SWR face.

#include "gui/CrossNeedleMeterGeometry.h"
#include "gui/CrossNeedleMeterWidget.h"

#include <QAccessible>
#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QEventLoop>
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

QPointF pointOnScale(const CrossNeedleMeterGeometry::Scale &scale, double angle) {
    return scale.center + QPointF(std::cos(angle), std::sin(angle)) * scale.radius;
}

const CrossNeedleMeterGeometry::SwrGuide *findGuide(
    const CrossNeedleMeterGeometry &geometry, const QString &label) {
    const auto guide = std::find_if(
        geometry.swrGuides.cbegin(), geometry.swrGuides.cend(),
        [&label](const CrossNeedleMeterGeometry::SwrGuide &candidate) {
            return candidate.label == label;
        });
    return guide == geometry.swrGuides.cend() ? nullptr : &*guide;
}

void testResourceAndConstruction() {
    QString error;
    const CrossNeedleMeterGeometry geometry = CrossNeedleMeterGeometry::loadResource(&error);
    report("V12 JSON resource validates", geometry.isValid(), error.toStdString());
    if (!geometry.isValid()) {
        return;
    }

    report("geometry format is versioned", geometry.formatVersion == 1);
    report("approved design version is retained", geometry.designVersion == 12);
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
    report("SWR fan retains all 12 source guides", geometry.swrGuides.size() == 12);
    int curvedGuideCount = 0;
    int straightGuideCount = 0;
    int cubicGuideCount = 0;
    bool tangentContinuous = true;
    bool registeredDatumsRetained = true;
    double maximumTopError = 0.0;
    double maximumVisibleBulge = 0.0;
    double minimumHiddenX = std::numeric_limits<double>::infinity();
    double maximumHiddenX = -std::numeric_limits<double>::infinity();
    for (const CrossNeedleMeterGeometry::SwrGuide &guide : geometry.swrGuides) {
        const QPainterPath path = geometry.swrGuidePath(guide);
        if (path.elementCount() != 7) {
            tangentContinuous = false;
            continue;
        }
        const QPainterPath::Element first = path.elementAt(0);
        const QPainterPath::Element visibleSecondControl = path.elementAt(2);
        const QPainterPath::Element datum = path.elementAt(3);
        const QPainterPath::Element hiddenFirstControl = path.elementAt(4);
        const QPainterPath::Element last = path.elementAt(path.elementCount() - 1);
        const double visibleYSpan = guide.registeredDatum.y() - guide.visibleUpper.y();
        const double bulge =
            std::abs(guide.quadraticA) * visibleYSpan * visibleYSpan / 4.0;
        maximumVisibleBulge = std::max(maximumVisibleBulge, bulge);
        if (bulge > 0.01) {
            ++curvedGuideCount;
        } else {
            ++straightGuideCount;
        }
        const bool allCubic =
            path.elementAt(1).type == QPainterPath::CurveToElement &&
            visibleSecondControl.type == QPainterPath::CurveToDataElement &&
            datum.type == QPainterPath::CurveToDataElement &&
            hiddenFirstControl.type == QPainterPath::CurveToElement &&
            path.elementAt(5).type == QPainterPath::CurveToDataElement &&
            last.type == QPainterPath::CurveToDataElement;
        if (allCubic) {
            ++cubicGuideCount;
        }
        const QPointF outgoing(datum.x - visibleSecondControl.x,
                               datum.y - visibleSecondControl.y);
        const QPointF incoming(hiddenFirstControl.x - datum.x,
                               hiddenFirstControl.y - datum.y);
        const double crossProduct = outgoing.x() * incoming.y() -
                                    outgoing.y() * incoming.x();
        const double tangentScale = std::hypot(outgoing.x(), outgoing.y()) *
                                    std::hypot(incoming.x(), incoming.y());
        tangentContinuous = tangentContinuous && tangentScale > 0.0 &&
                            std::abs(crossProduct) / tangentScale <= 1e-9 &&
                            QPointF::dotProduct(outgoing, incoming) > 0.0;
        registeredDatumsRetained =
            registeredDatumsRetained &&
            std::hypot(datum.x - guide.registeredDatum.x(),
                       datum.y - guide.registeredDatum.y()) <= 1e-9;
        maximumTopError =
            std::max(maximumTopError,
                     std::hypot(first.x - guide.visibleUpper.x(),
                                first.y - guide.visibleUpper.y()));
        minimumHiddenX = std::min(minimumHiddenX, last.x);
        maximumHiddenX = std::max(maximumHiddenX, last.x);
    }
    report("SWR fan retains the approved mix of straight and subtly curved guides",
           curvedGuideCount == 9 && straightGuideCount == 3 &&
               maximumVisibleBulge <= 6.01,
           "curved=" + std::to_string(curvedGuideCount) +
               " straight=" + std::to_string(straightGuideCount) +
               " max_bulge_px=" + std::to_string(maximumVisibleBulge));
    report("SWR contours retain registered V12 datums and C1 hidden continuations",
           cubicGuideCount == geometry.swrGuides.size() && tangentContinuous &&
               registeredDatumsRetained && maximumTopError <= 1e-9,
           "cubic=" + std::to_string(cubicGuideCount));
    report("lower mask conceals distinct source-fitted guide continuations",
           maximumHiddenX - minimumHiddenX > 100.0,
           "hidden_x_span=" + std::to_string(maximumHiddenX - minimumHiddenX));
    report("SWR contour ink survives applet-size downsampling",
           geometry.swrStyle.guideWidth >= 3.0);

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

    const QPointF intersection = geometry.needleIntersection(
        geometry.validation.activeForwardWatts, geometry.validation.activeReflectedWatts,
        geometry.validation.rangeMultiplier);
    const double intersectionError =
        std::hypot(intersection.x() - geometry.validation.intersection.x(),
                   intersection.y() - geometry.validation.intersection.y());
    report("active needle intersection matches approved proof", intersectionError < 0.01,
           "error_px=" + std::to_string(intersectionError));

    double guideDistance = 0.0;
    const QString nearestGuide = geometry.nearestGuideLabel(intersection, &guideDistance);
    report("active intersection lands on printed 1.5 guide",
           nearestGuide == geometry.validation.guide &&
               guideDistance <= geometry.validation.maximumGuideError,
           "guide=" + nearestGuide.toStdString() + " error_px=" + std::to_string(guideDistance));
    report("active proof retains the approved source-fitted guide error",
           std::abs(guideDistance - 1.0640493478502941) <= 0.05,
           "error_px=" + std::to_string(guideDistance));

    const CrossNeedleMeterGeometry::SwrGuide *activeGuide =
        findGuide(geometry, geometry.validation.guide);
    report("1.5 guide retains approved datum, curvature, and label anchor",
           activeGuide && near(activeGuide->registeredDatum.x(), 957.5, 1e-9) &&
               near(activeGuide->registeredDatum.y(), 880.0, 1e-9) &&
               near(activeGuide->quadraticA, -0.000102863228292, 1e-15) &&
               near(geometry.swrGuideLabelCenter(*activeGuide).x(), 1095.0, 1e-9) &&
               near(geometry.swrGuideLabelCenter(*activeGuide).y(), 690.0, 1e-9));
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
