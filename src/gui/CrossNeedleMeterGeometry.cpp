#include "CrossNeedleMeterGeometry.h"

#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace AetherSDR {

namespace {

QColor readColor(const QJsonValue &value, const QColor &fallback) {
    const QJsonArray a = value.toArray();
    if (a.size() != 4) {
        return fallback;
    }
    return QColor(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt());
}

QPointF readPoint(const QJsonValue &value, const QPointF &fallback = {}) {
    const QJsonArray a = value.toArray();
    if (a.size() != 2 || !a[0].isDouble() || !a[1].isDouble()) {
        return fallback;
    }
    return QPointF(a[0].toDouble(), a[1].toDouble());
}

QVector<double> readDoubles(const QJsonValue &value) {
    QVector<double> result;
    const QJsonArray a = value.toArray();
    result.reserve(a.size());
    for (const QJsonValue &entry : a) {
        if (!entry.isDouble()) {
            return {};
        }
        result.append(entry.toDouble());
    }
    return result;
}

QStringList readStrings(const QJsonValue &value) {
    QStringList result;
    const QJsonArray a = value.toArray();
    result.reserve(a.size());
    for (const QJsonValue &entry : a) {
        if (!entry.isString()) {
            return {};
        }
        result.append(entry.toString());
    }
    return result;
}

CrossNeedleMeterGeometry::Scale readScale(const QJsonObject &o) {
    CrossNeedleMeterGeometry::Scale scale;
    scale.center = readPoint(o.value(QStringLiteral("center")));
    scale.radius = o.value(QStringLiteral("radius")).toDouble();
    scale.startRadians = o.value(QStringLiteral("start_radians")).toDouble();
    scale.endRadians = o.value(QStringLiteral("end_radians")).toDouble();
    scale.values = readDoubles(o.value(QStringLiteral("values")));
    scale.referenceAnglesRadians =
        readDoubles(o.value(QStringLiteral("reference_angles_radians")));
    const QJsonObject response = o.value(QStringLiteral("response")).toObject();
    scale.responseModel = response.value(QStringLiteral("model")).toString();
    scale.responseStartRadians =
        response.value(QStringLiteral("start_radians")).toDouble();
    scale.responseEndRadians =
        response.value(QStringLiteral("end_radians")).toDouble();
    scale.responseCoefficients =
        readDoubles(response.value(QStringLiteral("coefficients")));
    scale.maximumReferenceErrorPixels =
        response.value(QStringLiteral("maximum_reference_error_pixels"))
            .toDouble(scale.maximumReferenceErrorPixels);
    scale.labels = readStrings(o.value(QStringLiteral("labels")));
    scale.minorSubdivisions = o.value(QStringLiteral("minor_subdivisions")).toInt(1);
    scale.labelOffset = o.value(QStringLiteral("label_offset")).toDouble(34.0);
    return scale;
}

bool finitePoint(const QPointF &point) {
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

double pointSegmentDistance(const QPointF &point, const QPointF &first, const QPointF &second) {
    const QPointF segment = second - first;
    const double lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared <= 1e-12) {
        return std::hypot(point.x() - first.x(), point.y() - first.y());
    }
    const double fraction =
        std::clamp(QPointF::dotProduct(point - first, segment) / lengthSquared, 0.0, 1.0);
    const QPointF closest = first + segment * fraction;
    return std::hypot(point.x() - closest.x(), point.y() - closest.y());
}

} // namespace

CrossNeedleMeterGeometry CrossNeedleMeterGeometry::loadResource(QString *error) {
    QFile file(QStringLiteral(":/meterfaces/cross-needle-v12.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot open :/meterfaces/cross-needle-v12.json");
        }
        return {};
    }
    return load(file, error);
}

CrossNeedleMeterGeometry CrossNeedleMeterGeometry::load(QIODevice &device, QString *error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(device.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
        }
        return {};
    }

    const QJsonObject root = document.object();
    CrossNeedleMeterGeometry geometry;
    geometry.formatVersion = root.value(QStringLiteral("format_version")).toInt();
    geometry.designVersion = root.value(QStringLiteral("design_version")).toInt();

    const QJsonObject canvas = root.value(QStringLiteral("canvas")).toObject();
    geometry.canvasWidth = canvas.value(QStringLiteral("width")).toDouble();
    geometry.canvasHeight = canvas.value(QStringLiteral("height")).toDouble();

    const QJsonObject frame = root.value(QStringLiteral("frame")).toObject();
    geometry.frame.outerInset = frame.value(QStringLiteral("outer_inset")).toDouble(6.0);
    geometry.frame.outerRadius = frame.value(QStringLiteral("outer_radius")).toDouble(23.0);
    geometry.frame.faceInset = frame.value(QStringLiteral("face_inset")).toDouble(26.0);
    geometry.frame.faceRadius = frame.value(QStringLiteral("face_radius")).toDouble(15.0);
    geometry.frame.faceOutlineWidth =
        frame.value(QStringLiteral("face_outline_width")).toDouble(5.0);
    geometry.frame.bezel =
        readColor(frame.value(QStringLiteral("bezel_rgba")), geometry.frame.bezel);
    geometry.frame.bezelEdge =
        readColor(frame.value(QStringLiteral("bezel_edge_rgba")), geometry.frame.bezelEdge);
    geometry.frame.face = readColor(frame.value(QStringLiteral("face_rgba")), geometry.frame.face);

    const QJsonObject faceGradient = root.value(QStringLiteral("face_gradient")).toObject();
    geometry.faceGradient.top =
        readColor(faceGradient.value(QStringLiteral("top_rgba")), geometry.faceGradient.top);
    geometry.faceGradient.middle =
        readColor(faceGradient.value(QStringLiteral("middle_rgba")), geometry.faceGradient.middle);
    geometry.faceGradient.bottom =
        readColor(faceGradient.value(QStringLiteral("bottom_rgba")), geometry.faceGradient.bottom);
    geometry.faceGradient.middleStop = faceGradient.value(QStringLiteral("middle_stop"))
                                           .toDouble(geometry.faceGradient.middleStop);
    geometry.faceGradient.glowCenter = readPoint(faceGradient.value(QStringLiteral("glow_center")),
                                                 geometry.faceGradient.glowCenter);
    geometry.faceGradient.glowRadius = faceGradient.value(QStringLiteral("glow_radius"))
                                           .toDouble(geometry.faceGradient.glowRadius);
    geometry.faceGradient.glowInner = readColor(
        faceGradient.value(QStringLiteral("glow_inner_rgba")), geometry.faceGradient.glowInner);
    geometry.faceGradient.glowOuter = readColor(
        faceGradient.value(QStringLiteral("glow_outer_rgba")), geometry.faceGradient.glowOuter);
    geometry.faceGradient.vignetteCenter =
        readPoint(faceGradient.value(QStringLiteral("vignette_center")),
                  geometry.faceGradient.vignetteCenter);
    geometry.faceGradient.vignetteRadius = faceGradient.value(QStringLiteral("vignette_radius"))
                                               .toDouble(geometry.faceGradient.vignetteRadius);
    geometry.faceGradient.vignetteClearStop =
        faceGradient.value(QStringLiteral("vignette_clear_stop"))
            .toDouble(geometry.faceGradient.vignetteClearStop);
    geometry.faceGradient.vignetteEdge =
        readColor(faceGradient.value(QStringLiteral("vignette_edge_rgba")),
                  geometry.faceGradient.vignetteEdge);

    const QJsonObject uplight = root.value(QStringLiteral("uplight_gradient")).toObject();
    geometry.uplightGradient.top =
        readColor(uplight.value(QStringLiteral("top_rgba")), geometry.uplightGradient.top);
    geometry.uplightGradient.middle =
        readColor(uplight.value(QStringLiteral("middle_rgba")), geometry.uplightGradient.middle);
    geometry.uplightGradient.bottom =
        readColor(uplight.value(QStringLiteral("bottom_rgba")), geometry.uplightGradient.bottom);
    geometry.uplightGradient.middleStop =
        uplight.value(QStringLiteral("middle_stop")).toDouble(geometry.uplightGradient.middleStop);
    geometry.uplightGradient.haloCenter = readPoint(uplight.value(QStringLiteral("halo_center")),
                                                    geometry.uplightGradient.haloCenter);
    geometry.uplightGradient.haloRadius =
        uplight.value(QStringLiteral("halo_radius")).toDouble(geometry.uplightGradient.haloRadius);
    geometry.uplightGradient.haloInner = readColor(uplight.value(QStringLiteral("halo_inner_rgba")),
                                                   geometry.uplightGradient.haloInner);
    geometry.uplightGradient.haloMiddle = readColor(
        uplight.value(QStringLiteral("halo_middle_rgba")), geometry.uplightGradient.haloMiddle);
    geometry.uplightGradient.haloMiddleStop =
        uplight.value(QStringLiteral("halo_middle_stop"))
            .toDouble(geometry.uplightGradient.haloMiddleStop);
    geometry.uplightGradient.haloShoulder = readColor(
        uplight.value(QStringLiteral("halo_shoulder_rgba")), geometry.uplightGradient.haloShoulder);
    geometry.uplightGradient.haloShoulderStop =
        uplight.value(QStringLiteral("halo_shoulder_stop"))
            .toDouble(geometry.uplightGradient.haloShoulderStop);
    geometry.uplightGradient.haloOuter = readColor(uplight.value(QStringLiteral("halo_outer_rgba")),
                                                   geometry.uplightGradient.haloOuter);
    geometry.uplightGradient.hotspotCenter = readPoint(
        uplight.value(QStringLiteral("hotspot_center")), geometry.uplightGradient.hotspotCenter);
    geometry.uplightGradient.hotspotRadius = uplight.value(QStringLiteral("hotspot_radius"))
                                                 .toDouble(geometry.uplightGradient.hotspotRadius);
    geometry.uplightGradient.hotspotInner = readColor(
        uplight.value(QStringLiteral("hotspot_inner_rgba")), geometry.uplightGradient.hotspotInner);
    geometry.uplightGradient.hotspotMiddle =
        readColor(uplight.value(QStringLiteral("hotspot_middle_rgba")),
                  geometry.uplightGradient.hotspotMiddle);
    geometry.uplightGradient.hotspotMiddleStop =
        uplight.value(QStringLiteral("hotspot_middle_stop"))
            .toDouble(geometry.uplightGradient.hotspotMiddleStop);
    geometry.uplightGradient.hotspotOuter = readColor(
        uplight.value(QStringLiteral("hotspot_outer_rgba")), geometry.uplightGradient.hotspotOuter);
    geometry.uplightGradient.bloomCenter = readPoint(
        uplight.value(QStringLiteral("bloom_center")), geometry.uplightGradient.bloomCenter);
    geometry.uplightGradient.bloomRadius =
        uplight.value(QStringLiteral("bloom_radius")).toDouble(geometry.uplightGradient.bloomRadius);
    geometry.uplightGradient.bloomInner = readColor(
        uplight.value(QStringLiteral("bloom_inner_rgba")), geometry.uplightGradient.bloomInner);
    geometry.uplightGradient.bloomMiddle = readColor(
        uplight.value(QStringLiteral("bloom_middle_rgba")), geometry.uplightGradient.bloomMiddle);
    geometry.uplightGradient.bloomMiddleStop =
        uplight.value(QStringLiteral("bloom_middle_stop"))
            .toDouble(geometry.uplightGradient.bloomMiddleStop);
    geometry.uplightGradient.bloomOuter = readColor(
        uplight.value(QStringLiteral("bloom_outer_rgba")), geometry.uplightGradient.bloomOuter);
    geometry.uplightGradient.paperGrainOpacity =
        uplight.value(QStringLiteral("paper_grain_opacity"))
            .toDouble(geometry.uplightGradient.paperGrainOpacity);
    geometry.uplightGradient.scaleSeparator =
        readColor(uplight.value(QStringLiteral("scale_separator_rgba")),
                  geometry.uplightGradient.scaleSeparator);
    geometry.uplightGradient.vignetteCenter = readPoint(
        uplight.value(QStringLiteral("vignette_center")), geometry.uplightGradient.vignetteCenter);
    geometry.uplightGradient.vignetteRadius =
        uplight.value(QStringLiteral("vignette_radius"))
            .toDouble(geometry.uplightGradient.vignetteRadius);
    geometry.uplightGradient.vignetteClearStop =
        uplight.value(QStringLiteral("vignette_clear_stop"))
            .toDouble(geometry.uplightGradient.vignetteClearStop);
    geometry.uplightGradient.vignetteEdge = readColor(
        uplight.value(QStringLiteral("vignette_edge_rgba")), geometry.uplightGradient.vignetteEdge);

    const QJsonObject dark = root.value(QStringLiteral("dark_theme")).toObject();
    geometry.darkTheme.top = readColor(dark.value(QStringLiteral("top_rgba")),
                                       geometry.darkTheme.top);
    geometry.darkTheme.middle = readColor(dark.value(QStringLiteral("middle_rgba")),
                                          geometry.darkTheme.middle);
    geometry.darkTheme.bottom = readColor(dark.value(QStringLiteral("bottom_rgba")),
                                          geometry.darkTheme.bottom);
    geometry.darkTheme.middleStop =
        dark.value(QStringLiteral("middle_stop")).toDouble(geometry.darkTheme.middleStop);
    geometry.darkTheme.ambientCenter = readPoint(dark.value(QStringLiteral("ambient_center")),
                                                 geometry.darkTheme.ambientCenter);
    geometry.darkTheme.ambientRadius =
        dark.value(QStringLiteral("ambient_radius")).toDouble(geometry.darkTheme.ambientRadius);
    geometry.darkTheme.ambientInner = readColor(dark.value(QStringLiteral("ambient_inner_rgba")),
                                                geometry.darkTheme.ambientInner);
    geometry.darkTheme.ambientOuter = readColor(dark.value(QStringLiteral("ambient_outer_rgba")),
                                                geometry.darkTheme.ambientOuter);
    geometry.darkTheme.glowCenter = readPoint(dark.value(QStringLiteral("glow_center")),
                                              geometry.darkTheme.glowCenter);
    geometry.darkTheme.glowRadius =
        dark.value(QStringLiteral("glow_radius")).toDouble(geometry.darkTheme.glowRadius);
    geometry.darkTheme.glowInner = readColor(dark.value(QStringLiteral("glow_inner_rgba")),
                                             geometry.darkTheme.glowInner);
    geometry.darkTheme.glowMiddle = readColor(dark.value(QStringLiteral("glow_middle_rgba")),
                                              geometry.darkTheme.glowMiddle);
    geometry.darkTheme.glowMiddleStop = dark.value(QStringLiteral("glow_middle_stop"))
                                            .toDouble(geometry.darkTheme.glowMiddleStop);
    geometry.darkTheme.glowOuter = readColor(dark.value(QStringLiteral("glow_outer_rgba")),
                                             geometry.darkTheme.glowOuter);
    geometry.darkTheme.vignetteCenter = readPoint(dark.value(QStringLiteral("vignette_center")),
                                                  geometry.darkTheme.vignetteCenter);
    geometry.darkTheme.vignetteRadius = dark.value(QStringLiteral("vignette_radius"))
                                            .toDouble(geometry.darkTheme.vignetteRadius);
    geometry.darkTheme.vignetteClearStop = dark.value(QStringLiteral("vignette_clear_stop"))
                                               .toDouble(geometry.darkTheme.vignetteClearStop);
    geometry.darkTheme.vignetteEdge = readColor(dark.value(QStringLiteral("vignette_edge_rgba")),
                                                geometry.darkTheme.vignetteEdge);
    geometry.darkTheme.paperGrainOpacity = dark.value(QStringLiteral("paper_grain_opacity"))
                                               .toDouble(geometry.darkTheme.paperGrainOpacity);
    geometry.darkTheme.scaleOuter = readColor(dark.value(QStringLiteral("scale_outer_rgba")),
                                              geometry.darkTheme.scaleOuter);
    geometry.darkTheme.scaleSeparator = readColor(
        dark.value(QStringLiteral("scale_separator_rgba")), geometry.darkTheme.scaleSeparator);
    geometry.darkTheme.scaleCalibration = readColor(
        dark.value(QStringLiteral("scale_calibration_rgba")), geometry.darkTheme.scaleCalibration);
    geometry.darkTheme.scaleInner = readColor(dark.value(QStringLiteral("scale_inner_rgba")),
                                              geometry.darkTheme.scaleInner);
    geometry.darkTheme.majorTick = readColor(dark.value(QStringLiteral("major_tick_rgba")),
                                             geometry.darkTheme.majorTick);
    geometry.darkTheme.minorTick = readColor(dark.value(QStringLiteral("minor_tick_rgba")),
                                             geometry.darkTheme.minorTick);
    geometry.darkTheme.text =
        readColor(dark.value(QStringLiteral("text_rgba")), geometry.darkTheme.text);
    geometry.darkTheme.rangeText = readColor(dark.value(QStringLiteral("range_text_rgba")),
                                             geometry.darkTheme.rangeText);
    geometry.darkTheme.swrGuide = readColor(dark.value(QStringLiteral("swr_guide_rgba")),
                                            geometry.darkTheme.swrGuide);
    geometry.darkTheme.swrLabel = readColor(dark.value(QStringLiteral("swr_label_rgba")),
                                            geometry.darkTheme.swrLabel);
    geometry.darkTheme.needle =
        readColor(dark.value(QStringLiteral("needle_rgba")), geometry.darkTheme.needle);
    geometry.darkTheme.needleEdge = readColor(dark.value(QStringLiteral("needle_edge_rgba")),
                                              geometry.darkTheme.needleEdge);
    geometry.darkTheme.needleHighlight = readColor(
        dark.value(QStringLiteral("needle_highlight_rgba")), geometry.darkTheme.needleHighlight);
    geometry.darkTheme.needleShadow = readColor(dark.value(QStringLiteral("needle_shadow_rgba")),
                                                geometry.darkTheme.needleShadow);
    geometry.darkTheme.maskFill = readColor(dark.value(QStringLiteral("mask_fill_rgba")),
                                            geometry.darkTheme.maskFill);
    geometry.darkTheme.maskEdge = readColor(dark.value(QStringLiteral("mask_edge_rgba")),
                                            geometry.darkTheme.maskEdge);
    geometry.darkTheme.maskText = readColor(dark.value(QStringLiteral("mask_text_rgba")),
                                            geometry.darkTheme.maskText);

    const QJsonObject style = root.value(QStringLiteral("scale_style")).toObject();
    geometry.scaleStyle.ribbonInset =
        style.value(QStringLiteral("ribbon_inset")).toDouble(geometry.scaleStyle.ribbonInset);
    geometry.scaleStyle.ribbonWidth =
        style.value(QStringLiteral("ribbon_width")).toDouble(geometry.scaleStyle.ribbonWidth);
    geometry.scaleStyle.ribbon =
        readColor(style.value(QStringLiteral("ribbon_rgba")), geometry.scaleStyle.ribbon);
    geometry.scaleStyle.outerWidth =
        style.value(QStringLiteral("outer_width")).toDouble(geometry.scaleStyle.outerWidth);
    geometry.scaleStyle.outer =
        readColor(style.value(QStringLiteral("outer_rgba")), geometry.scaleStyle.outer);
    geometry.scaleStyle.separatorInset =
        style.value(QStringLiteral("separator_inset")).toDouble(geometry.scaleStyle.separatorInset);
    geometry.scaleStyle.separatorWidth =
        style.value(QStringLiteral("separator_width")).toDouble(geometry.scaleStyle.separatorWidth);
    geometry.scaleStyle.separator =
        readColor(style.value(QStringLiteral("separator_rgba")), geometry.scaleStyle.separator);
    geometry.scaleStyle.calibrationInset = style.value(QStringLiteral("calibration_inset"))
                                               .toDouble(geometry.scaleStyle.calibrationInset);
    geometry.scaleStyle.calibrationWidth = style.value(QStringLiteral("calibration_width"))
                                               .toDouble(geometry.scaleStyle.calibrationWidth);
    geometry.scaleStyle.calibration =
        readColor(style.value(QStringLiteral("calibration_rgba")), geometry.scaleStyle.calibration);
    geometry.scaleStyle.innerInset =
        style.value(QStringLiteral("inner_inset")).toDouble(geometry.scaleStyle.innerInset);
    geometry.scaleStyle.innerWidth =
        style.value(QStringLiteral("inner_width")).toDouble(geometry.scaleStyle.innerWidth);
    geometry.scaleStyle.inner =
        readColor(style.value(QStringLiteral("inner_rgba")), geometry.scaleStyle.inner);
    geometry.scaleStyle.majorTickWidth = style.value(QStringLiteral("major_tick_width"))
                                             .toDouble(geometry.scaleStyle.majorTickWidth);
    geometry.scaleStyle.majorTick =
        readColor(style.value(QStringLiteral("major_tick_rgba")), geometry.scaleStyle.majorTick);
    geometry.scaleStyle.minorTickWidth = style.value(QStringLiteral("minor_tick_width"))
                                             .toDouble(geometry.scaleStyle.minorTickWidth);
    geometry.scaleStyle.minorTick =
        readColor(style.value(QStringLiteral("minor_tick_rgba")), geometry.scaleStyle.minorTick);
    geometry.scaleStyle.text =
        readColor(style.value(QStringLiteral("text_rgba")), geometry.scaleStyle.text);

    const QJsonObject typography = root.value(QStringLiteral("typography")).toObject();
    geometry.typography.scaleNumberPixels = typography.value(QStringLiteral("scale_numbers_px"))
                                                .toInt(geometry.typography.scaleNumberPixels);
    geometry.typography.sideTitlePixels = typography.value(QStringLiteral("side_titles_px"))
                                              .toInt(geometry.typography.sideTitlePixels);
    geometry.typography.rangePixels =
        typography.value(QStringLiteral("range_px")).toInt(geometry.typography.rangePixels);
    geometry.typography.unitPixels =
        typography.value(QStringLiteral("units_px")).toInt(geometry.typography.unitPixels);
    geometry.typography.swrNumberPixels = typography.value(QStringLiteral("swr_numbers_px"))
                                              .toInt(geometry.typography.swrNumberPixels);
    geometry.typography.maskLabelPixels = typography.value(QStringLiteral("mask_label_px"))
                                              .toInt(geometry.typography.maskLabelPixels);

    const QJsonObject range = root.value(QStringLiteral("range")).toObject();
    geometry.rangeMultipliers = readDoubles(range.value(QStringLiteral("multipliers")));
    geometry.rangeLabel = range.value(QStringLiteral("label")).toString();
    geometry.rangeLabelCenter =
        readPoint(range.value(QStringLiteral("label_center")), geometry.rangeLabelCenter);

    geometry.forwardScale = readScale(root.value(QStringLiteral("forward_scale")).toObject());
    geometry.reflectedScale = readScale(root.value(QStringLiteral("reflected_scale")).toObject());
    const auto deriveTickAngles = [](Scale &scale) {
        scale.anglesRadians.clear();
        scale.anglesRadians.reserve(scale.values.size());
        for (const double value : scale.values) {
            scale.anglesRadians.append(CrossNeedleMeterGeometry::angleForValue(scale, value));
        }
    };
    deriveTickAngles(geometry.forwardScale);
    deriveTickAngles(geometry.reflectedScale);

    const QJsonObject titles = root.value(QStringLiteral("titles")).toObject();
    const QJsonObject forwardTitle = titles.value(QStringLiteral("forward")).toObject();
    geometry.forwardTitle.text = forwardTitle.value(QStringLiteral("text")).toString();
    geometry.forwardTitle.center = readPoint(forwardTitle.value(QStringLiteral("center")));
    geometry.forwardTitle.rotationDegrees =
        forwardTitle.value(QStringLiteral("rotation_degrees")).toDouble();
    const QJsonObject reflectedTitle = titles.value(QStringLiteral("reflected")).toObject();
    geometry.reflectedTitle.text = reflectedTitle.value(QStringLiteral("text")).toString();
    geometry.reflectedTitle.center = readPoint(reflectedTitle.value(QStringLiteral("center")));
    geometry.reflectedTitle.rotationDegrees =
        reflectedTitle.value(QStringLiteral("rotation_degrees")).toDouble();
    geometry.forwardUnitCenter =
        readPoint(titles.value(QStringLiteral("forward_unit_center")), geometry.forwardUnitCenter);
    geometry.reflectedUnitCenter = readPoint(titles.value(QStringLiteral("reflected_unit_center")),
                                             geometry.reflectedUnitCenter);

    const QJsonObject overlap = root.value(QStringLiteral("scale_overlap")).toObject();
    geometry.scaleOverlap.reflectedBehindForward =
        overlap.value(QStringLiteral("reflected_behind_forward")).toBool(true);
    geometry.scaleOverlap.reflectedGapCenterRadians =
        overlap.value(QStringLiteral("reflected_gap_center_radians"))
            .toDouble(geometry.scaleOverlap.reflectedGapCenterRadians);
    geometry.scaleOverlap.reflectedGapHalfSpanRadians =
        overlap.value(QStringLiteral("reflected_gap_half_span_radians"))
            .toDouble(geometry.scaleOverlap.reflectedGapHalfSpanRadians);
    geometry.scaleOverlap.reflectedGapCenterRadiusInset =
        overlap.value(QStringLiteral("reflected_gap_center_radius_inset"))
            .toDouble(geometry.scaleOverlap.reflectedGapCenterRadiusInset);
    geometry.scaleOverlap.reflectedGapWidth =
        overlap.value(QStringLiteral("reflected_gap_width"))
            .toDouble(geometry.scaleOverlap.reflectedGapWidth);

    const QJsonObject swr = root.value(QStringLiteral("swr")).toObject();
    geometry.swrStyle.guide =
        readColor(swr.value(QStringLiteral("guide_rgba")), geometry.swrStyle.guide);
    geometry.swrStyle.guideWidth =
        swr.value(QStringLiteral("guide_width")).toDouble(geometry.swrStyle.guideWidth);
    geometry.swrStyle.label =
        readColor(swr.value(QStringLiteral("label_rgba")), geometry.swrStyle.label);
    geometry.swrStyle.graphClearance =
        swr.value(QStringLiteral("graph_clearance"))
            .toDouble(geometry.swrStyle.graphClearance);
    geometry.swrStyle.maskGap =
        swr.value(QStringLiteral("mask_gap")).toDouble(geometry.swrStyle.maskGap);
    geometry.swrStyle.curveSamples =
        swr.value(QStringLiteral("curve_samples")).toInt(geometry.swrStyle.curveSamples);
    geometry.swrStyle.labelArcFraction =
        swr.value(QStringLiteral("label_arc_fraction")).toDouble(geometry.swrStyle.labelArcFraction);
    geometry.swrStyle.labelDeclutterStep =
        swr.value(QStringLiteral("label_declutter_step"))
            .toDouble(geometry.swrStyle.labelDeclutterStep);
    geometry.swrStyle.labelBoxPadding =
        swr.value(QStringLiteral("label_box_padding")).toDouble(geometry.swrStyle.labelBoxPadding);
    const QJsonArray guides = swr.value(QStringLiteral("guides")).toArray();
    geometry.swrGuides.reserve(guides.size());
    for (const QJsonValue &value : guides) {
        const QJsonObject o = value.toObject();
        SwrGuide guide;
        guide.label = o.value(QStringLiteral("label")).toString();
        guide.displayLabel = o.value(QStringLiteral("display_label")).toString();
        const QJsonValue swrValue = o.value(QStringLiteral("swr"));
        guide.swr = swrValue.isString() &&
                            swrValue.toString() == QStringLiteral("infinity")
                        ? std::numeric_limits<double>::infinity()
                        : swrValue.toDouble();
        geometry.swrGuides.append(guide);
    }

    const QJsonObject needles = root.value(QStringLiteral("needles")).toObject();
    geometry.needleStyle.line =
        readColor(needles.value(QStringLiteral("line_rgba")), geometry.needleStyle.line);
    geometry.needleStyle.lineWidth = needles.value(QStringLiteral("line_width")).toDouble(6.0);
    geometry.needleStyle.edge =
        readColor(needles.value(QStringLiteral("edge_rgba")), geometry.needleStyle.edge);
    geometry.needleStyle.edgeWidth =
        needles.value(QStringLiteral("edge_width")).toDouble(geometry.needleStyle.edgeWidth);
    geometry.needleStyle.edgeOffset =
        needles.value(QStringLiteral("edge_offset")).toDouble(geometry.needleStyle.edgeOffset);
    geometry.needleStyle.highlight = readColor(needles.value(QStringLiteral("highlight_rgba")),
                                               geometry.needleStyle.highlight);
    geometry.needleStyle.highlightWidth = needles.value(QStringLiteral("highlight_width"))
                                              .toDouble(geometry.needleStyle.highlightWidth);
    geometry.needleStyle.highlightOffset = needles.value(QStringLiteral("highlight_offset"))
                                               .toDouble(geometry.needleStyle.highlightOffset);
    geometry.needleStyle.shadow =
        readColor(needles.value(QStringLiteral("shadow_rgba")), geometry.needleStyle.shadow);
    geometry.needleStyle.shadowWidth = needles.value(QStringLiteral("shadow_width"))
                                           .toDouble(geometry.needleStyle.shadowWidth);
    geometry.needleStyle.shadowOffset = readPoint(needles.value(QStringLiteral("shadow_offset")),
                                                  geometry.needleStyle.shadowOffset);
    geometry.needleStyle.softShadow = readColor(
        needles.value(QStringLiteral("soft_shadow_rgba")), geometry.needleStyle.softShadow);
    geometry.needleStyle.softShadowWidth =
        needles.value(QStringLiteral("soft_shadow_width"))
            .toDouble(geometry.needleStyle.softShadowWidth);
    geometry.needleStyle.softShadowOffset = readPoint(
        needles.value(QStringLiteral("soft_shadow_offset")),
        geometry.needleStyle.softShadowOffset);

    const QJsonObject mask = root.value(QStringLiteral("mask")).toObject();
    const QJsonArray boundary = mask.value(QStringLiteral("boundary")).toArray();
    geometry.mask.boundary.reserve(boundary.size());
    for (const QJsonValue &value : boundary) {
        geometry.mask.boundary.append(readPoint(value));
    }
    geometry.mask.bottomY = mask.value(QStringLiteral("bottom_y")).toDouble(974.0);
    geometry.mask.fill = readColor(mask.value(QStringLiteral("fill_rgba")), geometry.mask.fill);
    geometry.mask.edge = readColor(mask.value(QStringLiteral("edge_rgba")), geometry.mask.edge);
    geometry.mask.text = readColor(mask.value(QStringLiteral("text_rgba")), geometry.mask.text);
    geometry.mask.label = mask.value(QStringLiteral("label")).toString(QStringLiteral("SWR"));
    geometry.mask.labelCenter =
        readPoint(mask.value(QStringLiteral("label_center")), geometry.mask.labelCenter);

    const QJsonObject validation = root.value(QStringLiteral("validation")).toObject();
    geometry.validation.activeForwardWatts =
        validation.value(QStringLiteral("active_forward_watts")).toDouble(100.0);
    geometry.validation.activeSwr = validation.value(QStringLiteral("active_swr")).toDouble(1.5);
    geometry.validation.activeReflectedWatts =
        validation.value(QStringLiteral("active_reflected_watts")).toDouble(4.0);
    geometry.validation.rangeMultiplier =
        validation.value(QStringLiteral("range_multiplier")).toDouble(10.0);
    geometry.validation.intersection = readPoint(validation.value(QStringLiteral("intersection")),
                                                 geometry.validation.intersection);
    geometry.validation.guide =
        validation.value(QStringLiteral("guide")).toString(QStringLiteral("1.5"));
    geometry.validation.maximumGuideError =
        validation.value(QStringLiteral("maximum_guide_error")).toDouble(2.0);

    QString validationError;
    if (!geometry.isValid(&validationError)) {
        if (error) {
            *error = validationError;
        }
        return {};
    }
    return geometry;
}

CrossNeedleMeterGeometry CrossNeedleMeterGeometry::fallback() {
    CrossNeedleMeterGeometry geometry;
    geometry.formatVersion = 6;
    geometry.designVersion = 19;
    geometry.rangeLabel = QStringLiteral("RANGE  20 W x1   200 W x10   2 kW x100");
    geometry.forwardTitle = {QStringLiteral("FORWARD"), QPointF(180.0, 545.0), -58.0};
    geometry.reflectedTitle = {QStringLiteral("REFLECTED"), QPointF(1320.0, 545.0), 58.0};
    geometry.forwardScale.center = QPointF(1099.0, 1057.0);
    geometry.forwardScale.radius = 952.5350893203638;
    geometry.forwardScale.startRadians = -2.922529943067865;
    geometry.forwardScale.endRadians = -1.6309954106531042;
    geometry.forwardScale.values = {0.0, 20.0};
    geometry.forwardScale.referenceAnglesRadians = {-2.922529943067865,
                                                     -1.7009954106531042};
    geometry.forwardScale.responseModel = QStringLiteral("concave_bernstein_v1");
    geometry.forwardScale.responseStartRadians = -2.922529943067865;
    geometry.forwardScale.responseEndRadians = -1.7009954106531042;
    geometry.forwardScale.responseCoefficients = {
        0.0, 0.4833365491938737, 0.6144073900045368,
        0.7454782308151997, 0.8727391154075999, 1.0};
    geometry.forwardScale.maximumReferenceErrorPixels = 1.0;
    geometry.forwardScale.anglesRadians = {
        angleForValue(geometry.forwardScale, 0.0),
        angleForValue(geometry.forwardScale, 20.0)};
    geometry.forwardScale.labels = {QStringLiteral("0"), QStringLiteral("20")};
    geometry.forwardScale.minorSubdivisions = 5;
    geometry.reflectedScale.center = QPointF(401.0, 1057.0);
    geometry.reflectedScale.radius = 952.5350893203638;
    geometry.reflectedScale.startRadians = -0.21906271052192816;
    geometry.reflectedScale.endRadians = -1.510597242936689;
    geometry.reflectedScale.values = {0.0, 4.0};
    geometry.reflectedScale.referenceAnglesRadians = {-0.21906271052192816,
                                                       -1.440597242936689};
    geometry.reflectedScale.responseModel = QStringLiteral("concave_bernstein_v1");
    geometry.reflectedScale.responseStartRadians = -0.21906271052192816;
    geometry.reflectedScale.responseEndRadians = -1.440597242936689;
    geometry.reflectedScale.responseCoefficients = {
        0.0, 0.26723624915834737, 0.5344724983166947,
        0.7871286394467959, 0.8935643197233979, 1.0};
    geometry.reflectedScale.maximumReferenceErrorPixels = 1.0;
    geometry.reflectedScale.anglesRadians = {
        angleForValue(geometry.reflectedScale, 0.0),
        angleForValue(geometry.reflectedScale, 4.0)};
    geometry.reflectedScale.labels = {QStringLiteral("0"), QStringLiteral("4")};
    geometry.reflectedScale.minorSubdivisions = 2;
    geometry.swrStyle.graphClearance = 60.0;
    geometry.swrStyle.curveSamples = 128;
    geometry.swrGuides = {{QStringLiteral("1.5"), QStringLiteral("1.5"), 1.5}};
    geometry.mask.boundary = {{52.0, 904.0},   {250.0, 904.0},  {500.0, 900.0},
                              {650.0, 880.0},  {750.0, 872.0},  {850.0, 880.0},
                              {1000.0, 900.0}, {1250.0, 904.0}, {1448.0, 904.0}};
    return geometry;
}

bool CrossNeedleMeterGeometry::isValid(QString *error) const {
    auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (formatVersion != 6) {
        return fail(QStringLiteral("unsupported cross-needle geometry format version"));
    }
    if (designVersion <= 0) {
        return fail(QStringLiteral("cross-needle design version is invalid"));
    }
    if (!(canvasWidth > 0.0 && canvasHeight > 0.0) || !std::isfinite(canvasWidth) ||
        !std::isfinite(canvasHeight)) {
        return fail(QStringLiteral("cross-needle canvas is invalid"));
    }
    if (!finitePoint(faceGradient.glowCenter) || !finitePoint(faceGradient.vignetteCenter) ||
        !(faceGradient.glowRadius > 0.0) || !std::isfinite(faceGradient.glowRadius) ||
        !(faceGradient.vignetteRadius > 0.0) || !std::isfinite(faceGradient.vignetteRadius) ||
        !(faceGradient.middleStop > 0.0 && faceGradient.middleStop < 1.0) ||
        !(faceGradient.vignetteClearStop >= 0.0 && faceGradient.vignetteClearStop < 1.0)) {
        return fail(QStringLiteral("cross-needle face gradient is invalid"));
    }
    if (!finitePoint(uplightGradient.haloCenter) || !finitePoint(uplightGradient.hotspotCenter) ||
        !finitePoint(uplightGradient.bloomCenter) || !finitePoint(uplightGradient.vignetteCenter) ||
        !(uplightGradient.haloRadius > 0.0) ||
        !std::isfinite(uplightGradient.haloRadius) || !(uplightGradient.hotspotRadius > 0.0) ||
        !std::isfinite(uplightGradient.hotspotRadius) || !(uplightGradient.bloomRadius > 0.0) ||
        !std::isfinite(uplightGradient.bloomRadius) || !(uplightGradient.vignetteRadius > 0.0) ||
        !std::isfinite(uplightGradient.vignetteRadius) ||
        !(uplightGradient.middleStop > 0.0 && uplightGradient.middleStop < 1.0) ||
        !(uplightGradient.haloMiddleStop > 0.0 && uplightGradient.haloMiddleStop < 1.0) ||
        !(uplightGradient.haloShoulderStop > uplightGradient.haloMiddleStop &&
          uplightGradient.haloShoulderStop < 1.0) ||
        !(uplightGradient.hotspotMiddleStop > 0.0 && uplightGradient.hotspotMiddleStop < 1.0) ||
        !(uplightGradient.bloomMiddleStop > 0.0 && uplightGradient.bloomMiddleStop < 1.0) ||
        !(uplightGradient.paperGrainOpacity >= 0.0 &&
          uplightGradient.paperGrainOpacity <= 0.30) ||
        !(uplightGradient.vignetteClearStop >= 0.0 && uplightGradient.vignetteClearStop < 1.0)) {
        return fail(QStringLiteral("cross-needle uplight gradient is invalid"));
    }
    if (!finitePoint(darkTheme.ambientCenter) || !finitePoint(darkTheme.glowCenter) ||
        !finitePoint(darkTheme.vignetteCenter) || !(darkTheme.ambientRadius > 0.0) ||
        !std::isfinite(darkTheme.ambientRadius) || !(darkTheme.glowRadius > 0.0) ||
        !std::isfinite(darkTheme.glowRadius) || !(darkTheme.vignetteRadius > 0.0) ||
        !std::isfinite(darkTheme.vignetteRadius) ||
        !(darkTheme.middleStop > 0.0 && darkTheme.middleStop < 1.0) ||
        !(darkTheme.glowMiddleStop > 0.0 && darkTheme.glowMiddleStop < 1.0) ||
        !(darkTheme.vignetteClearStop >= 0.0 && darkTheme.vignetteClearStop < 1.0) ||
        !(darkTheme.paperGrainOpacity >= 0.0 && darkTheme.paperGrainOpacity <= 0.30)) {
        return fail(QStringLiteral("cross-needle dark theme is invalid"));
    }
    const auto positiveFinite = [](double value) { return value > 0.0 && std::isfinite(value); };
    const auto nonNegativeFinite = [](double value) {
        return value >= 0.0 && std::isfinite(value);
    };
    if (!nonNegativeFinite(scaleStyle.ribbonInset) || !positiveFinite(scaleStyle.ribbonWidth) ||
        !positiveFinite(scaleStyle.outerWidth) || !nonNegativeFinite(scaleStyle.separatorInset) ||
        !positiveFinite(scaleStyle.separatorWidth) ||
        !nonNegativeFinite(scaleStyle.calibrationInset) ||
        !positiveFinite(scaleStyle.calibrationWidth) || !nonNegativeFinite(scaleStyle.innerInset) ||
        !positiveFinite(scaleStyle.innerWidth) || !positiveFinite(scaleStyle.majorTickWidth) ||
        !positiveFinite(scaleStyle.minorTickWidth)) {
        return fail(QStringLiteral("cross-needle scale style is invalid"));
    }
    if (typography.scaleNumberPixels <= 0 || typography.sideTitlePixels <= 0 ||
        typography.rangePixels <= 0 || typography.unitPixels <= 0 ||
        typography.swrNumberPixels <= 0 || typography.maskLabelPixels <= 0) {
        return fail(QStringLiteral("cross-needle typography is invalid"));
    }
    if (!positiveFinite(needleStyle.lineWidth) || !positiveFinite(needleStyle.edgeWidth) ||
        !nonNegativeFinite(needleStyle.edgeOffset) ||
        !positiveFinite(needleStyle.highlightWidth) ||
        !nonNegativeFinite(needleStyle.highlightOffset) ||
        !positiveFinite(needleStyle.shadowWidth) || !finitePoint(needleStyle.shadowOffset) ||
        !positiveFinite(needleStyle.softShadowWidth) ||
        !finitePoint(needleStyle.softShadowOffset) ||
        needleStyle.edgeWidth >= needleStyle.lineWidth ||
        needleStyle.highlightWidth >= needleStyle.lineWidth ||
        needleStyle.softShadowWidth <= needleStyle.shadowWidth) {
        return fail(QStringLiteral("cross-needle needle material is invalid"));
    }
    const auto validScale = [](const Scale &scale) {
        if (!finitePoint(scale.center) || !(scale.radius > 0.0) ||
            !std::isfinite(scale.radius) || scale.values.size() < 2 ||
            scale.values.size() != scale.anglesRadians.size() ||
            scale.values.size() != scale.referenceAnglesRadians.size() ||
            scale.responseModel != QStringLiteral("concave_bernstein_v1") ||
            scale.responseCoefficients.size() != 6 ||
            scale.values.size() != scale.labels.size() || scale.minorSubdivisions < 1) {
            return false;
        }
        const bool anglesIncrease =
            scale.anglesRadians.last() > scale.anglesRadians.first();
        for (int index = 1; index < scale.values.size(); ++index) {
            if (!(scale.values[index] > scale.values[index - 1]) ||
                !std::isfinite(scale.anglesRadians[index]) ||
                !std::isfinite(scale.referenceAnglesRadians[index]) ||
                ((scale.anglesRadians[index] > scale.anglesRadians[index - 1]) !=
                 anglesIncrease) ||
                ((scale.referenceAnglesRadians[index] >
                  scale.referenceAnglesRadians[index - 1]) != anglesIncrease)) {
                return false;
            }
        }
        if (!std::isfinite(scale.anglesRadians.first()) ||
            !std::isfinite(scale.referenceAnglesRadians.first()) ||
            !std::isfinite(scale.responseStartRadians) ||
            !std::isfinite(scale.responseEndRadians) ||
            scale.responseEndRadians == scale.responseStartRadians ||
            (scale.responseEndRadians > scale.responseStartRadians) != anglesIncrease ||
            !(scale.maximumReferenceErrorPixels > 0.0) ||
            !std::isfinite(scale.maximumReferenceErrorPixels) ||
            std::abs(scale.responseCoefficients.first()) > 1e-12 ||
            std::abs(scale.responseCoefficients.last() - 1.0) > 1e-12) {
            return false;
        }
        for (int index = 0; index < scale.responseCoefficients.size(); ++index) {
            const double coefficient = scale.responseCoefficients[index];
            if (!std::isfinite(coefficient) || coefficient < 0.0 || coefficient > 1.0 ||
                (index > 0 &&
                 coefficient + 1e-12 < scale.responseCoefficients[index - 1])) {
                return false;
            }
        }
        // Concave control polygon (non-positive second differences): sensitivity
        // decreases with power, so calibration noise cannot knot the contours.
        for (int index = 0; index + 2 < scale.responseCoefficients.size(); ++index) {
            const double secondDifference = scale.responseCoefficients[index + 2] -
                                            2.0 * scale.responseCoefficients[index + 1] +
                                            scale.responseCoefficients[index];
            if (secondDifference > 1e-12) {
                return false;
            }
        }
        for (int index = 0; index < scale.values.size(); ++index) {
            if (std::abs(CrossNeedleMeterGeometry::angleForValue(
                             scale, scale.values[index]) -
                         scale.anglesRadians[index]) > 1e-12) {
                return false;
            }
        }
        return true;
    };
    if (!validScale(forwardScale) || !validScale(reflectedScale)) {
        return fail(QStringLiteral("cross-needle power scale arrays are invalid"));
    }
    const auto scaleFitIsValid = [](const Scale &scale) {
        // The needle parks on the printed zero (angled rest), so every tick
        // including 0 is constrained by the photographed calibration.
        for (int index = 0; index < scale.values.size(); ++index) {
            const double fittedAngle =
                CrossNeedleMeterGeometry::angleForValue(scale, scale.values[index]);
            const double errorPixels =
                std::abs(fittedAngle - scale.referenceAnglesRadians[index]) * scale.radius;
            if (!std::isfinite(errorPixels) ||
                errorPixels > scale.maximumReferenceErrorPixels) {
                return false;
            }
        }
        return true;
    };
    if (!scaleFitIsValid(forwardScale) || !scaleFitIsValid(reflectedScale)) {
        return fail(QStringLiteral("cross-needle movement fit exceeds tick tolerance"));
    }
    if (!std::isfinite(scaleOverlap.reflectedGapCenterRadians) ||
        !(scaleOverlap.reflectedGapHalfSpanRadians > 0.0 &&
          scaleOverlap.reflectedGapHalfSpanRadians < 0.05) ||
        !(scaleOverlap.reflectedGapCenterRadiusInset >= 0.0) ||
        !(scaleOverlap.reflectedGapWidth > 0.0)) {
        return fail(QStringLiteral("cross-needle scale overlap is invalid"));
    }
    if (swrGuides.isEmpty() || !(swrStyle.graphClearance > 0.0) ||
        swrStyle.graphClearance >= std::min(forwardScale.radius, reflectedScale.radius) ||
        swrStyle.curveSamples < 32 || swrStyle.curveSamples > 1024 ||
        !(swrStyle.labelArcFraction > 0.0) || !(swrStyle.labelArcFraction <= 1.0) ||
        !(swrStyle.labelDeclutterStep > 0.0) || !std::isfinite(swrStyle.labelDeclutterStep) ||
        !(swrStyle.labelBoxPadding >= 0.0) || !std::isfinite(swrStyle.labelBoxPadding) ||
        !(swrStyle.maskGap >= 0.0) || !std::isfinite(swrStyle.maskGap)) {
        return fail(QStringLiteral("cross-needle SWR construction settings are invalid"));
    }
    bool validMaskBoundary = mask.boundary.size() >= 3;
    double previousMaskX = -std::numeric_limits<double>::infinity();
    double maximumMaskY = -std::numeric_limits<double>::infinity();
    for (const QPointF &point : mask.boundary) {
        validMaskBoundary = validMaskBoundary && finitePoint(point) &&
                            point.x() >= 0.0 && point.x() <= canvasWidth &&
                            point.y() >= 0.0 && point.y() <= canvasHeight &&
                            point.x() > previousMaskX;
        previousMaskX = point.x();
        maximumMaskY = std::max(maximumMaskY, point.y());
    }
    for (qsizetype index = 0; index < mask.boundary.size(); ++index) {
        const QPointF &left = mask.boundary[index];
        const QPointF &right = mask.boundary[mask.boundary.size() - 1 - index];
        validMaskBoundary = validMaskBoundary &&
                            std::abs(left.x() + right.x() - canvasWidth) <= 0.01 &&
                            std::abs(left.y() - right.y()) <= 0.01;
    }
    if (!validMaskBoundary || !std::isfinite(mask.bottomY) ||
        mask.bottomY < maximumMaskY || mask.bottomY > canvasHeight ||
        mask.label.trimmed().isEmpty() || !finitePoint(mask.labelCenter) ||
        mask.labelCenter.x() < 0.0 || mask.labelCenter.x() > canvasWidth ||
        mask.labelCenter.y() < 0.0 || mask.labelCenter.y() > canvasHeight) {
        return fail(QStringLiteral("cross-needle lower mask is invalid"));
    }
    swrLabelCenterCache.clear();
    swrLabelCenterCacheFontKey.clear();
    swrLabelPlacementError.clear();
    QFont swrLabelFont;
    if (QGuiApplication::instance()) {
        swrLabelFont = QGuiApplication::font();
    }
    swrLabelFont.setPixelSize(typography.swrNumberPixels);
    swrLabelFont.setBold(true);
    for (const SwrGuide &guide : swrGuides) {
        const bool validSwr = guide.swr > 1.0;
        const bool validLabel =
            guide.displayLabel.isEmpty() || !guide.displayLabel.trimmed().isEmpty();
        if (guide.label.isEmpty() || !validSwr || !validLabel) {
            return fail(QStringLiteral("cross-needle SWR guide '%1' is invalid").arg(guide.label));
        }
        const QPainterPath path = swrGuidePath(guide);
        if (path.elementCount() != swrStyle.curveSamples + 1 ||
            path.elementAt(0).type != QPainterPath::MoveToElement ||
            !finitePoint(path.currentPosition())) {
            return fail(QStringLiteral("cross-needle SWR guide '%1' path is invalid "
                                       "(%2 of %3 samples)")
                            .arg(guide.label)
                            .arg(path.elementCount())
                            .arg(swrStyle.curveSamples + 1));
        }
        if (!guide.displayLabel.isEmpty()) {
            const QPointF labelCenter = swrGuideLabelCenter(guide, swrLabelFont);
            if (!swrLabelPlacementError.isEmpty()) {
                return fail(swrLabelPlacementError);
            }
            if (!finitePoint(labelCenter) || labelCenter.x() <= 0.0 ||
                labelCenter.x() >= canvasWidth || labelCenter.y() <= 0.0 ||
                labelCenter.y() >= canvasHeight) {
                return fail(QStringLiteral("cross-needle SWR guide '%1' label is outside the face")
                                .arg(guide.label));
            }
        }
    }
    if (!swrLabelPlacementError.isEmpty()) {
        return fail(swrLabelPlacementError);
    }
    return true;
}

double CrossNeedleMeterGeometry::angleForValue(const Scale &scale, double value) {
    if (scale.values.size() < 2 || scale.responseCoefficients.size() != 6) {
        return 0.0;
    }
    const double span = scale.values.last() - scale.values.first();
    const double normalizedPower =
        span > 0.0 ? std::clamp((value - scale.values.first()) / span, 0.0, 1.0) : 0.0;
    return angleForNormalizedPower(scale, normalizedPower);
}

double CrossNeedleMeterGeometry::angleForNormalizedPower(const Scale &scale,
                                                         double normalizedPower) {
    if (scale.responseCoefficients.size() != 6) {
        return scale.responseStartRadians;
    }
    // Degree-5 Bernstein in normalized POWER (concave, square-root-like),
    // evaluated by de Casteljau. normalizedPower 0 maps to the angled
    // printed-zero rest. Callers pass values in [0, 1] for live/tick use;
    // SWR-contour construction passes values > 1 to extrapolate a movement a
    // little past full scale so every contour can reach the common termination
    // envelope (see swrGuidePath / docs/cross-needle-meter-math.md D1).
    std::array<double, 6> work{};
    std::copy(scale.responseCoefficients.cbegin(), scale.responseCoefficients.cend(),
              work.begin());
    for (int remaining = static_cast<int>(scale.responseCoefficients.size()) - 1;
         remaining > 0; --remaining) {
        for (int index = 0; index < remaining; ++index) {
            work[index] = std::lerp(work[index], work[index + 1], normalizedPower);
        }
    }
    return std::lerp(scale.responseStartRadians, scale.responseEndRadians, work[0]);
}

double CrossNeedleMeterGeometry::printedAngleForIndex(const Scale &scale, int index) {
    if (index < 0 || index >= scale.anglesRadians.size()) {
        return 0.0;
    }
    // The needle parks on the printed zero, so every tick (including 0) uses
    // the calibrated movement angle directly.
    return scale.anglesRadians[index];
}

double CrossNeedleMeterGeometry::inverseInterpolate(const Scale &scale,
                                                    double angleRadians) {
    if (scale.values.size() < 2 || scale.values.size() != scale.anglesRadians.size() ||
        scale.responseCoefficients.size() != 6) {
        return 0.0;
    }

    const bool increasing = scale.anglesRadians.last() > scale.anglesRadians.first();
    const double minimumAngle =
        std::min(scale.anglesRadians.first(), scale.anglesRadians.last());
    const double maximumAngle =
        std::max(scale.anglesRadians.first(), scale.anglesRadians.last());
    const double target = std::clamp(angleRadians, minimumAngle, maximumAngle);
    double lower = scale.values.first();
    double upper = scale.values.last();
    for (int iteration = 0; iteration < 64; ++iteration) {
        const double middle = (lower + upper) * 0.5;
        const double angle = angleForValue(scale, middle);
        if ((angle < target) == increasing) {
            lower = middle;
        } else {
            upper = middle;
        }
    }
    return (lower + upper) * 0.5;
}

QPointF CrossNeedleMeterGeometry::pointOnScale(const Scale &scale, double angleRadians) {
    return scale.center + QPointF(std::cos(angleRadians), std::sin(angleRadians)) * scale.radius;
}

double CrossNeedleMeterGeometry::forwardAngle(double forwardWatts, double multiplier) const {
    const double safeMultiplier = multiplier > 0.0 ? multiplier : 1.0;
    return angleForValue(forwardScale, std::max(0.0, forwardWatts) / safeMultiplier);
}

double CrossNeedleMeterGeometry::reflectedAngle(double reflectedWatts, double multiplier) const {
    const double safeMultiplier = multiplier > 0.0 ? multiplier : 1.0;
    return angleForValue(reflectedScale, std::max(0.0, reflectedWatts) / safeMultiplier);
}

QPointF CrossNeedleMeterGeometry::forwardTip(double forwardWatts, double multiplier) const {
    return pointOnScale(forwardScale, forwardAngle(forwardWatts, multiplier));
}

QPointF CrossNeedleMeterGeometry::reflectedTip(double reflectedWatts, double multiplier) const {
    return pointOnScale(reflectedScale, reflectedAngle(reflectedWatts, multiplier));
}

QPointF CrossNeedleMeterGeometry::lineIntersection(const QPointF &firstOrigin,
                                                   const QPointF &firstTip,
                                                   const QPointF &secondOrigin,
                                                   const QPointF &secondTip) {
    const double x1 = firstOrigin.x();
    const double y1 = firstOrigin.y();
    const double x2 = firstTip.x();
    const double y2 = firstTip.y();
    const double x3 = secondOrigin.x();
    const double y3 = secondOrigin.y();
    const double x4 = secondTip.x();
    const double y4 = secondTip.y();
    const double denominator = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
    if (std::abs(denominator) < 1e-12) {
        return QPointF(std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::quiet_NaN());
    }
    const double firstCross = x1 * y2 - y1 * x2;
    const double secondCross = x3 * y4 - y3 * x4;
    return QPointF((firstCross * (x3 - x4) - (x1 - x2) * secondCross) / denominator,
                   (firstCross * (y3 - y4) - (y1 - y2) * secondCross) / denominator);
}

QPointF CrossNeedleMeterGeometry::needleIntersection(double forwardWatts, double reflectedWatts,
                                                     double multiplier) const {
    return lineIntersection(forwardScale.center, forwardTip(forwardWatts, multiplier),
                            reflectedScale.center, reflectedTip(reflectedWatts, multiplier));
}

QPainterPath CrossNeedleMeterGeometry::swrGuidePath(const SwrGuide &guide) const {
    QPainterPath path;
    if (!(guide.swr > 1.0) || swrStyle.curveSamples < 2) {
        return path;
    }

    // A cross-needle SWR guide is the locus of intersections produced by a
    // constant reflected/forward power ratio. Sample in detector-voltage
    // space, not power space: the movement input is proportional to sqrt(P),
    // and uniform voltage steps avoid over-tessellating one part of a curve
    // while starving another.
    const double ratio = std::isinf(guide.swr)
                             ? 1.0
                             : std::pow((guide.swr - 1.0) / (guide.swr + 1.0), 2.0);
    const double forwardMaximum = forwardScale.values.last();
    const double reflectedMaximum = reflectedScale.values.last();

    // Every contour terminates on ONE common boundary: a fixed clearance short
    // of whichever power arc is nearer. Low-SWR crossings reach that boundary
    // only a little past full forward scale, so we extend the movement angles
    // slightly past unity (angleForNormalizedPower, unclamped) to get there.
    // This makes the whole SWR family a consistent fan parallel to the arcs
    // (see docs/cross-needle-meter-math.md D1). Live needles are unaffected.
    const auto point = [this, ratio, forwardMaximum, reflectedMaximum](double voltage) {
        const double forwardNormalized = voltage * voltage;
        const double reflectedNormalized =
            forwardNormalized * forwardMaximum * ratio / reflectedMaximum;
        return lineIntersection(
            forwardScale.center,
            pointOnScale(forwardScale,
                         angleForNormalizedPower(forwardScale, forwardNormalized)),
            reflectedScale.center,
            pointOnScale(reflectedScale,
                         angleForNormalizedPower(reflectedScale, reflectedNormalized)));
    };
    const auto reachesGraphEnvelope = [this](const QPointF &p) {
        const double forwardDistance =
            std::hypot(p.x() - forwardScale.center.x(), p.y() - forwardScale.center.y());
        const double reflectedDistance =
            std::hypot(p.x() - reflectedScale.center.x(), p.y() - reflectedScale.center.y());
        return forwardDistance >= forwardScale.radius - swrStyle.graphClearance ||
               reflectedDistance >= reflectedScale.radius - swrStyle.graphClearance;
    };

    // Cap on how far past full scale a movement may be extrapolated to reach
    // the boundary (~2.5x full-scale power). Step outward from the hidden
    // convergence to the FIRST envelope crossing, then bisect for precision.
    constexpr double kMaxVoltage = 1.6;
    double endingVoltage = kMaxVoltage;
    double previous = 0.0;
    constexpr int kProbe = 256;
    for (int i = 1; i <= kProbe; ++i) {
        const double v = kMaxVoltage * static_cast<double>(i) / kProbe;
        if (reachesGraphEnvelope(point(v))) {
            double lower = previous;
            double upper = v;
            for (int iteration = 0; iteration < 48; ++iteration) {
                const double middle = (lower + upper) * 0.5;
                if (reachesGraphEnvelope(point(middle))) {
                    upper = middle;
                } else {
                    lower = middle;
                }
            }
            endingVoltage = (lower + upper) * 0.5;
            break;
        }
        previous = v;
    }

    // The needles rest on their printed zeros (angled rest), so voltage 0 is a
    // well-defined crossing: the single hidden convergence just below the mask
    // that every contour fans from. Start the path there and sweep to the
    // common boundary.
    for (int sample = 0; sample <= swrStyle.curveSamples; ++sample) {
        const double fraction =
            static_cast<double>(sample) / static_cast<double>(swrStyle.curveSamples);
        const QPointF p = point(endingVoltage * fraction);
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) {
            return {};
        }
        if (sample == 0) {
            path.moveTo(p);
        } else {
            path.lineTo(p);
        }
    }
    return path;
}

double CrossNeedleMeterGeometry::maskBoundaryY(double x) const {
    if (mask.boundary.isEmpty()) {
        return mask.bottomY;
    }
    if (x <= mask.boundary.first().x()) {
        return mask.boundary.first().y();
    }
    for (int index = 1; index < mask.boundary.size(); ++index) {
        const QPointF &first = mask.boundary[index - 1];
        const QPointF &second = mask.boundary[index];
        if (x <= second.x()) {
            if (second.x() == first.x()) {
                return std::min(first.y(), second.y());
            }
            const double fraction = (x - first.x()) / (second.x() - first.x());
            return first.y() + (second.y() - first.y()) * fraction;
        }
    }
    return mask.boundary.last().y();
}

QRectF CrossNeedleMeterGeometry::swrLabelBox(
    const QPointF &center, const SwrGuide &guide, const QFont &labelFont) const
{
    const QString display = guide.displayLabel == QStringLiteral("infinity")
                                ? QString::fromUtf8("\xe2\x88\x9e")
                                : guide.displayLabel;
    QRectF box;
    if (QGuiApplication::instance()) {
        // Match the rendered number exactly (same font, size, weight, and the
        // small background inset the painter and tests use) so a declutter
        // decision made here is the one the device sees.
        box = QFontMetricsF(labelFont)
                  .boundingRect(display)
                  .adjusted(-3.0, -2.0, 3.0, 2.0);
    } else {
        // Headless fallback: a proportional estimate a touch larger than the
        // glyphs, so decisions stay conservative without a font available.
        const double pixels = static_cast<double>(typography.swrNumberPixels);
        double width = 0.0;
        for (const QChar character : display) {
            width += character == QLatin1Char('.') ? 0.36 * pixels
                     : display == QString::fromUtf8("\xe2\x88\x9e") ? 1.05 * pixels
                                                                    : 0.62 * pixels;
        }
        box = QRectF(0.0, 0.0, width + 6.0, 1.30 * pixels);
    }
    box.moveCenter(center);
    return box;
}

QVector<QPointF> CrossNeedleMeterGeometry::swrLabelCenters(const QFont &labelFont) const {
    const QString fontKey = labelFont.key();
    if (swrLabelCenterCache.size() == swrGuides.size() &&
        swrLabelCenterCacheFontKey == fontKey) {
        return swrLabelCenterCache;
    }
    swrLabelPlacementError.clear();
    // Every number rides its own contour: the anchor is a fraction
    // (labelArcFraction) of that contour's VISIBLE arc length, then decluttered.
    // No per-guide coordinates.
    QVector<QPointF> centers(swrGuides.size(),
                             QPointF(std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::quiet_NaN()));
    QVector<QPainterPath> paths(swrGuides.size());
    for (int index = 0; index < swrGuides.size(); ++index) {
        paths[index] = swrGuidePath(swrGuides[index]);
    }

    const auto pointAtDistance = [](const QPainterPath &path, double target, QPointF *tangent) {
        if (path.isEmpty()) {
            return QPointF(std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::quiet_NaN());
        }
        double traversed = 0.0;
        QPointF first = path.elementAt(0);
        for (int element = 1; element < path.elementCount(); ++element) {
            const QPointF second = path.elementAt(element);
            const double segment = std::hypot(second.x() - first.x(), second.y() - first.y());
            if (traversed + segment >= target && segment > 1e-12) {
                if (tangent) {
                    *tangent = (second - first) / segment;
                }
                return first + (second - first) * ((target - traversed) / segment);
            }
            traversed += segment;
            first = second;
        }
        QPointF finalTangent(0.0, -1.0);
        if (path.elementCount() >= 2) {
            const QPointF before = path.elementAt(path.elementCount() - 2);
            const QPointF end = path.currentPosition();
            const double segment = std::hypot(end.x() - before.x(), end.y() - before.y());
            finalTangent = segment > 1e-12 ? (end - before) / segment : QPointF(0.0, -1.0);
        }
        if (tangent) {
            *tangent = finalTangent;
        }
        return path.currentPosition() + finalTangent * std::max(0.0, target - traversed);
    };

    QVector<QRectF> powerNumberBoxes;
    if (QGuiApplication::instance()) {
        QFont scaleFont = labelFont;
        scaleFont.setPixelSize(typography.scaleNumberPixels);
        scaleFont.setWeight(QFont::Medium);
        const QFontMetricsF metrics(scaleFont);
        const auto appendScaleBoxes = [&](const Scale &scale) {
            for (int index = 0; index < scale.labels.size(); ++index) {
                if (scale.labels[index].isEmpty()) {
                    continue;
                }
                const double angle = printedAngleForIndex(scale, index);
                const QPointF radial(std::cos(angle), std::sin(angle));
                const QPointF center = pointOnScale(scale, angle) + radial * scale.labelOffset;
                QRectF box = metrics.boundingRect(scale.labels[index]);
                box.moveCenter(center);
                powerNumberBoxes.append(box);
            }
        };
        appendScaleBoxes(forwardScale);
        appendScaleBoxes(reflectedScale);
    }

    const double gap = swrStyle.labelBoxPadding;
    QVector<QRectF> placed;
    placed.reserve(swrGuides.size());
    for (int index = 0; index < swrGuides.size(); ++index) {
        const SwrGuide &guide = swrGuides[index];
        const auto boxClears = [&](const QPointF &candidate) {
            const QRectF box = swrLabelBox(candidate, guide, labelFont);
            if (!(box.bottom() + 3.0 < maskBoundaryY(box.left()) &&
                  box.bottom() + 3.0 < maskBoundaryY(box.center().x()) &&
                  box.bottom() + 3.0 < maskBoundaryY(box.right()) &&
                  box.left() > frame.faceInset && box.right() < canvasWidth - frame.faceInset &&
                  box.top() > frame.faceInset && box.bottom() < mask.bottomY)) {
                return false;
            }
            const QRectF spaced = box.adjusted(-gap, -gap, gap, gap);
            for (const QRectF &other : placed) {
                if (spaced.intersects(other)) {
                    return false;
                }
            }
            for (const QRectF &powerBox : powerNumberBoxes) {
                if (spaced.intersects(powerBox)) {
                    return false;
                }
            }
            for (int other = 0; other < paths.size(); ++other) {
                if (other != index && paths[other].intersects(spaced)) {
                    return false;
                }
            }
            return true;
        };
        // Anchor within the VISIBLE portion of the contour (above the mask):
        // walk from the mask crossing a fraction of the visible arc length, so
        // short low-SWR contours and long high-SWR contours label in the same
        // relative near-the-outer-end region rather than in the hidden crowd.
        double totalLength = 0.0;
        double visibleStart = -1.0;
        for (int element = 1; element < paths[index].elementCount(); ++element) {
            const QPointF a = paths[index].elementAt(element - 1);
            const QPointF b = paths[index].elementAt(element);
            if (visibleStart < 0.0 && b.y() < maskBoundaryY(b.x())) {
                visibleStart = totalLength;
            }
            totalLength += std::hypot(b.x() - a.x(), b.y() - a.y());
        }
        if (visibleStart < 0.0) {
            visibleStart = 0.0;
        }
        const double baseDistance =
            visibleStart + swrStyle.labelArcFraction * (totalLength - visibleStart);
        bool found = false;
        for (int attempt = 0; attempt <= 256; ++attempt) {
            const int magnitude = (attempt + 1) / 2;
            const int direction = attempt == 0 ? 0 : (attempt % 2 == 1 ? 1 : -1);
            // No upper clamp: distances beyond the contour length extrapolate
            // along the upper tangent, letting a crowded low-SWR label (e.g.
            // 1.1, whose visible stub is too short to host a box) ride up its
            // own continuation into open space while staying nearest itself.
            const double distance = std::max(
                baseDistance + direction * magnitude * swrStyle.labelDeclutterStep, 0.0);
            QPointF tangent;
            const QPointF anchor = pointAtDistance(paths[index], distance, &tangent);
            const QPointF normal(-tangent.y(), tangent.x());
            for (int normalAttempt = 0; normalAttempt <= 128; ++normalAttempt) {
                const int normalMagnitude = (normalAttempt + 1) / 2;
                const int normalDirection =
                    normalAttempt == 0 ? 0 : (normalAttempt % 2 == 1 ? 1 : -1);
                const QPointF candidate =
                    anchor + normal * (normalDirection * normalMagnitude *
                                       swrStyle.labelDeclutterStep);
                if (boxClears(candidate)) {
                    centers[index] = candidate;
                    placed.append(swrLabelBox(candidate, guide, labelFont));
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (!found) {
            swrLabelPlacementError =
                QStringLiteral("cross-needle SWR guide '%1' has no collision-free label position")
                    .arg(guide.label);
            break;
        }
    }
    swrLabelCenterCache = centers;
    swrLabelCenterCacheFontKey = fontKey;
    return centers;
}

QPointF CrossNeedleMeterGeometry::swrGuideLabelCenter(
    const SwrGuide &guide, const QFont &labelFont) const
{
    if (guide.displayLabel.isEmpty()) {
        return {};
    }
    const QVector<QPointF> centers = swrLabelCenters(labelFont);
    for (int index = 0; index < swrGuides.size(); ++index) {
        if (swrGuides[index].label == guide.label) {
            return centers[index];
        }
    }
    return {};
}

QPointF CrossNeedleMeterGeometry::swrGuideUpperEndpoint(const SwrGuide &guide) const {
    return swrGuidePath(guide).currentPosition();
}

QPointF CrossNeedleMeterGeometry::powerReadingsAtIntersection(
    const QPointF &point, double rangeMultiplier) const {
    const double safeMultiplier = rangeMultiplier > 0.0 ? rangeMultiplier : 1.0;
    const double forwardAngleRadians =
        std::atan2(point.y() - forwardScale.center.y(),
                   point.x() - forwardScale.center.x());
    const double reflectedAngleRadians =
        std::atan2(point.y() - reflectedScale.center.y(),
                   point.x() - reflectedScale.center.x());
    return QPointF(inverseInterpolate(forwardScale, forwardAngleRadians) * safeMultiplier,
                   inverseInterpolate(reflectedScale, reflectedAngleRadians) * safeMultiplier);
}

double CrossNeedleMeterGeometry::distanceToGuide(const QPointF &point,
                                                 const SwrGuide &guide) const {
    const QPainterPath path = swrGuidePath(guide);
    if (path.isEmpty()) {
        return std::numeric_limits<double>::infinity();
    }

    constexpr int kDistanceSamples = 640;
    double minimum = std::numeric_limits<double>::infinity();
    QPointF previous = path.pointAtPercent(0.0);
    for (int i = 1; i <= kDistanceSamples; ++i) {
        const QPointF current =
            path.pointAtPercent(static_cast<double>(i) / kDistanceSamples);
        minimum = std::min(minimum, pointSegmentDistance(point, previous, current));
        previous = current;
    }
    return minimum;
}

QString CrossNeedleMeterGeometry::nearestGuideLabel(const QPointF &point, double *distance) const {
    QString nearest;
    double minimum = std::numeric_limits<double>::infinity();
    for (const SwrGuide &guide : swrGuides) {
        const double candidate = distanceToGuide(point, guide);
        if (candidate < minimum) {
            minimum = candidate;
            nearest = guide.label;
        }
    }
    if (distance) {
        *distance = minimum;
    }
    return nearest;
}

double CrossNeedleMeterGeometry::reflectedPowerWatts(double forwardWatts, double swr) {
    if (!std::isfinite(forwardWatts) || forwardWatts <= 0.0 || std::isnan(swr) || swr <= 1.0) {
        return 0.0;
    }
    if (std::isinf(swr)) {
        return forwardWatts;
    }
    const double rho = std::clamp((swr - 1.0) / (swr + 1.0), 0.0, 1.0);
    return forwardWatts * rho * rho;
}

double CrossNeedleMeterGeometry::swrFromPowers(double forwardWatts, double reflectedWatts) {
    if (!std::isfinite(forwardWatts) || !std::isfinite(reflectedWatts) || forwardWatts <= 0.0 ||
        reflectedWatts <= 0.0) {
        return 1.0;
    }
    if (reflectedWatts >= forwardWatts) {
        return std::numeric_limits<double>::infinity();
    }
    const double rho = std::sqrt(reflectedWatts / forwardWatts);
    return (1.0 + rho) / (1.0 - rho);
}

double CrossNeedleMeterGeometry::rangeMultiplierFor(int maxWatts, bool amplifierActive) {
    if (amplifierActive || maxWatts > 200) {
        return 100.0;
    }
    if (maxWatts > 20) {
        return 10.0;
    }
    return 1.0;
}

} // namespace AetherSDR
