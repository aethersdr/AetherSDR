#include "CrossNeedleMeterGeometry.h"

#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>

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

QVector<double> monotoneTangents(const QVector<double> &values,
                                 const QVector<double> &outputs) {
    const int count = values.size();
    if (count < 2 || outputs.size() != count) {
        return {};
    }

    QVector<double> intervals(count - 1);
    QVector<double> slopes(count - 1);
    for (int i = 0; i + 1 < count; ++i) {
        intervals[i] = values[i + 1] - values[i];
        if (!(intervals[i] > 0.0)) {
            return {};
        }
        slopes[i] = (outputs[i + 1] - outputs[i]) / intervals[i];
    }

    QVector<double> tangents(count);
    if (count == 2) {
        tangents[0] = slopes[0];
        tangents[1] = slopes[0];
        return tangents;
    }

    for (int i = 1; i + 1 < count; ++i) {
        if (slopes[i - 1] * slopes[i] <= 0.0) {
            tangents[i] = 0.0;
            continue;
        }
        const double firstWeight = 2.0 * intervals[i] + intervals[i - 1];
        const double secondWeight = intervals[i] + 2.0 * intervals[i - 1];
        tangents[i] = (firstWeight + secondWeight) /
                      (firstWeight / slopes[i - 1] + secondWeight / slopes[i]);
    }

    const auto endpointTangent = [](double firstInterval, double secondInterval,
                                    double firstSlope, double secondSlope) {
        double tangent = ((2.0 * firstInterval + secondInterval) * firstSlope -
                          firstInterval * secondSlope) /
                         (firstInterval + secondInterval);
        if (tangent * firstSlope <= 0.0) {
            return 0.0;
        }
        if (firstSlope * secondSlope < 0.0 &&
            std::abs(tangent) > 3.0 * std::abs(firstSlope)) {
            tangent = 3.0 * firstSlope;
        }
        return tangent;
    };
    tangents[0] = endpointTangent(intervals[0], intervals[1], slopes[0], slopes[1]);
    tangents[count - 1] = endpointTangent(intervals[count - 2], intervals[count - 3],
                                          slopes[count - 2], slopes[count - 3]);
    return tangents;
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
    scale.anglesRadians = readDoubles(o.value(QStringLiteral("angles_radians")));
    scale.angleTangents = monotoneTangents(scale.values, scale.anglesRadians);
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
        guide.visibleUpper = readPoint(o.value(QStringLiteral("visible_upper")));
        guide.registeredDatum = readPoint(o.value(QStringLiteral("registered_datum")));
        guide.hiddenLower = readPoint(o.value(QStringLiteral("hidden_lower")));
        guide.quadraticA = o.value(QStringLiteral("quadratic_a")).toDouble();
        guide.labelCenter = readPoint(o.value(QStringLiteral("label_center")));
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
    geometry.formatVersion = 1;
    geometry.designVersion = 12;
    geometry.rangeLabel = QStringLiteral("RANGE  20 W x1   200 W x10   2 kW x100");
    geometry.forwardTitle = {QStringLiteral("FORWARD"), QPointF(180.0, 545.0), -58.0};
    geometry.reflectedTitle = {QStringLiteral("REFLECTED"), QPointF(1320.0, 545.0), 58.0};
    geometry.forwardScale.center = QPointF(1099.0, 1057.0);
    geometry.forwardScale.radius = 952.5350893203638;
    geometry.forwardScale.startRadians = -2.922529943067865;
    geometry.forwardScale.endRadians = -1.6309954106531042;
    geometry.forwardScale.values = {0.0, 20.0};
    geometry.forwardScale.anglesRadians = {-2.922529943067865, -1.7009954106531042};
    geometry.forwardScale.angleTangents =
        monotoneTangents(geometry.forwardScale.values, geometry.forwardScale.anglesRadians);
    geometry.forwardScale.labels = {QStringLiteral("0"), QStringLiteral("20")};
    geometry.forwardScale.minorSubdivisions = 5;
    geometry.reflectedScale.center = QPointF(401.0, 1057.0);
    geometry.reflectedScale.radius = 952.5350893203638;
    geometry.reflectedScale.startRadians = -0.21906271052192816;
    geometry.reflectedScale.endRadians = -1.510597242936689;
    geometry.reflectedScale.values = {0.0, 4.0};
    geometry.reflectedScale.anglesRadians = {-0.21906271052192816, -1.440597242936689};
    geometry.reflectedScale.angleTangents =
        monotoneTangents(geometry.reflectedScale.values, geometry.reflectedScale.anglesRadians);
    geometry.reflectedScale.labels = {QStringLiteral("0"), QStringLiteral("4")};
    geometry.reflectedScale.minorSubdivisions = 2;
    geometry.swrGuides = {
        {QStringLiteral("1.5"), QStringLiteral("1.5"), 1.5,
         QPointF(1049.4649081096002, 591.025257178578), QPointF(957.5, 880.0),
         QPointF(887.4859925937714, 1100.0), -0.000102863228292,
         QPointF(1095.0, 690.0)}};
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

    if (formatVersion != 1) {
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
        return finitePoint(scale.center) && scale.radius > 0.0 && std::isfinite(scale.radius) &&
               scale.values.size() >= 2 && scale.values.size() == scale.anglesRadians.size() &&
               scale.values.size() == scale.angleTangents.size() &&
               scale.values.size() == scale.labels.size() &&
               std::is_sorted(scale.values.cbegin(), scale.values.cend()) &&
               std::all_of(scale.anglesRadians.cbegin(), scale.anglesRadians.cend(),
                           [](double angle) { return std::isfinite(angle); }) &&
               std::all_of(scale.angleTangents.cbegin(), scale.angleTangents.cend(),
                           [](double tangent) { return std::isfinite(tangent); }) &&
               scale.minorSubdivisions >= 1;
    };
    if (!validScale(forwardScale) || !validScale(reflectedScale)) {
        return fail(QStringLiteral("cross-needle power scale arrays are invalid"));
    }
    if (!std::isfinite(scaleOverlap.reflectedGapCenterRadians) ||
        !(scaleOverlap.reflectedGapHalfSpanRadians > 0.0 &&
          scaleOverlap.reflectedGapHalfSpanRadians < 0.05) ||
        !(scaleOverlap.reflectedGapCenterRadiusInset >= 0.0) ||
        !(scaleOverlap.reflectedGapWidth > 0.0)) {
        return fail(QStringLiteral("cross-needle scale overlap is invalid"));
    }
    if (swrGuides.isEmpty()) {
        return fail(QStringLiteral("cross-needle SWR guide set is empty"));
    }
    for (const SwrGuide &guide : swrGuides) {
        const bool validSwr = guide.swr > 1.0;
        const bool validConstruction =
            finitePoint(guide.visibleUpper) && finitePoint(guide.registeredDatum) &&
            finitePoint(guide.hiddenLower) && std::isfinite(guide.quadraticA) &&
            guide.visibleUpper.y() > 0.0 && guide.visibleUpper.y() < canvasHeight &&
            guide.visibleUpper.y() < guide.registeredDatum.y() &&
            guide.registeredDatum.y() < guide.hiddenLower.y();
        const bool validLabel =
            guide.displayLabel.isEmpty() ||
            (finitePoint(guide.labelCenter) && guide.labelCenter.x() > 0.0 &&
             guide.labelCenter.x() < canvasWidth && guide.labelCenter.y() > 0.0 &&
             guide.labelCenter.y() < canvasHeight);
        const double visibleYSpan = guide.registeredDatum.y() - guide.visibleUpper.y();
        const double maximumVisibleBulge =
            std::abs(guide.quadraticA) * visibleYSpan * visibleYSpan / 4.0;
        if (guide.label.isEmpty() || !validSwr || !validConstruction || !validLabel ||
            maximumVisibleBulge > 6.01) {
            return fail(QStringLiteral("cross-needle SWR guide '%1' is invalid").arg(guide.label));
        }
        const QPainterPath path = swrGuidePath(guide);
        if (path.elementCount() != 7 || path.elementAt(0).type != QPainterPath::MoveToElement ||
            path.elementAt(1).type != QPainterPath::CurveToElement ||
            path.elementAt(4).type != QPainterPath::CurveToElement) {
            return fail(QStringLiteral("cross-needle SWR guide '%1' path is invalid")
                            .arg(guide.label));
        }
    }
    if (mask.boundary.size() < 3) {
        return fail(QStringLiteral("cross-needle lower mask is invalid"));
    }
    return true;
}

double CrossNeedleMeterGeometry::interpolate(const Scale &scale, double value) {
    if (scale.values.size() < 2 || scale.values.size() != scale.anglesRadians.size() ||
        scale.values.size() != scale.angleTangents.size()) {
        return 0.0;
    }
    const double clamped = std::clamp(value, scale.values.first(), scale.values.last());
    const auto upper = std::lower_bound(scale.values.cbegin(), scale.values.cend(), clamped);
    if (upper == scale.values.cbegin()) {
        return scale.anglesRadians.first();
    }
    if (upper == scale.values.cend()) {
        return scale.anglesRadians.last();
    }
    const int upperIndex = static_cast<int>(std::distance(scale.values.cbegin(), upper));
    const int lowerIndex = upperIndex - 1;
    const double span = scale.values[upperIndex] - scale.values[lowerIndex];
    const double fraction = (clamped - scale.values[lowerIndex]) / span;
    const double fractionSquared = fraction * fraction;
    const double fractionCubed = fractionSquared * fraction;
    const double lowerBasis = 2.0 * fractionCubed - 3.0 * fractionSquared + 1.0;
    const double lowerTangentBasis = fractionCubed - 2.0 * fractionSquared + fraction;
    const double upperBasis = -2.0 * fractionCubed + 3.0 * fractionSquared;
    const double upperTangentBasis = fractionCubed - fractionSquared;
    return lowerBasis * scale.anglesRadians[lowerIndex] +
           lowerTangentBasis * span * scale.angleTangents[lowerIndex] +
           upperBasis * scale.anglesRadians[upperIndex] +
           upperTangentBasis * span * scale.angleTangents[upperIndex];
}

QPointF CrossNeedleMeterGeometry::pointOnScale(const Scale &scale, double angleRadians) {
    return scale.center + QPointF(std::cos(angleRadians), std::sin(angleRadians)) * scale.radius;
}

double CrossNeedleMeterGeometry::forwardAngle(double forwardWatts, double multiplier) const {
    const double safeMultiplier = multiplier > 0.0 ? multiplier : 1.0;
    return interpolate(forwardScale, std::max(0.0, forwardWatts) / safeMultiplier);
}

double CrossNeedleMeterGeometry::reflectedAngle(double reflectedWatts, double multiplier) const {
    const double safeMultiplier = multiplier > 0.0 ? multiplier : 1.0;
    return interpolate(reflectedScale, std::max(0.0, reflectedWatts) / safeMultiplier);
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
    const double visibleYSpan = guide.registeredDatum.y() - guide.visibleUpper.y();
    const double hiddenYSpan = guide.hiddenLower.y() - guide.registeredDatum.y();
    if (!(visibleYSpan > 0.0 && hiddenYSpan > 0.0)) {
        return path;
    }

    // Port of the approved V12 construction. The visible source-traced
    // quadratic is represented exactly as a cubic Hermite segment. Its end
    // tangent then feeds the concealed Hermite continuation, so there is no
    // splice at the mask boundary and no mechanically invented S-curve.
    const double visibleChordSlope =
        (guide.registeredDatum.x() - guide.visibleUpper.x()) / visibleYSpan;
    const double upperSlope = visibleChordSlope - guide.quadraticA * visibleYSpan;
    const double datumSlope = visibleChordSlope + guide.quadraticA * visibleYSpan;
    const double lowerSlope =
        (guide.hiddenLower.x() - guide.registeredDatum.x()) / hiddenYSpan;

    path.moveTo(guide.visibleUpper);
    path.cubicTo(
        guide.visibleUpper + QPointF(upperSlope * visibleYSpan / 3.0,
                                     visibleYSpan / 3.0),
        guide.registeredDatum - QPointF(datumSlope * visibleYSpan / 3.0,
                                        visibleYSpan / 3.0),
        guide.registeredDatum);
    path.cubicTo(
        guide.registeredDatum + QPointF(datumSlope * hiddenYSpan / 3.0,
                                        hiddenYSpan / 3.0),
        guide.hiddenLower - QPointF(lowerSlope * hiddenYSpan / 3.0,
                                    hiddenYSpan / 3.0),
        guide.hiddenLower);
    return path;
}

QPointF CrossNeedleMeterGeometry::swrGuideLabelCenter(const SwrGuide &guide) const {
    if (guide.displayLabel.isEmpty()) {
        return {};
    }
    return guide.labelCenter;
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
