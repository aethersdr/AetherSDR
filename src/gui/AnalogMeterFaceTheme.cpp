#include "AnalogMeterFaceTheme.h"

#include <QFile>
#include <QImage>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kFormatVersion = 1;

bool readFiniteDouble(const QJsonObject& object, const QString& key,
                      double& output, QString& error)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        error = QStringLiteral("%1 must be a finite number").arg(key);
        return false;
    }
    output = value.toDouble();
    return true;
}

bool readPoint(const QJsonObject& object, const QString& key,
               QPointF& output, QString& error)
{
    const QJsonValue value = object.value(key);
    if (!value.isArray()) {
        error = QStringLiteral("%1 must be a two-number array").arg(key);
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() != 2 || !array[0].isDouble() || !array[1].isDouble()
        || !std::isfinite(array[0].toDouble()) || !std::isfinite(array[1].toDouble())) {
        error = QStringLiteral("%1 must be a two-number array").arg(key);
        return false;
    }
    output = QPointF(array[0].toDouble(), array[1].toDouble());
    return true;
}

bool readRect(const QJsonObject& object, const QString& key,
              QRectF& output, QString& error)
{
    const QJsonValue value = object.value(key);
    if (!value.isArray()) {
        error = QStringLiteral("%1 must be a four-number array").arg(key);
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() != 4) {
        error = QStringLiteral("%1 must be a four-number array").arg(key);
        return false;
    }
    for (const QJsonValue& entry : array) {
        if (!entry.isDouble() || !std::isfinite(entry.toDouble())) {
            error = QStringLiteral("%1 must be a four-number array").arg(key);
            return false;
        }
    }
    output = QRectF(array[0].toDouble(), array[1].toDouble(),
                    array[2].toDouble(), array[3].toDouble());
    return true;
}

bool readColor(const QJsonObject& object, const QString& key,
               QColor& output, QString& error)
{
    const QJsonValue value = object.value(key);
    if (!value.isArray()) {
        error = QStringLiteral("%1 must be an RGBA array").arg(key);
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() != 4) {
        error = QStringLiteral("%1 must be an RGBA array").arg(key);
        return false;
    }
    int channels[4]{};
    for (int index = 0; index < 4; ++index) {
        if (!array[index].isDouble()) {
            error = QStringLiteral("%1 must be an RGBA array").arg(key);
            return false;
        }
        channels[index] = array[index].toInt(-1);
        if (channels[index] < 0 || channels[index] > 255) {
            error = QStringLiteral("%1 contains an invalid channel").arg(key);
            return false;
        }
    }
    output = QColor(channels[0], channels[1], channels[2], channels[3]);
    return true;
}

bool readPalette(const QJsonObject& object,
                 AnalogMeterFaceThemeCatalog::Palette& palette,
                 QString& error)
{
    return readColor(object, QStringLiteral("ribbon_rgba"), palette.ribbon, error)
        && readColor(object, QStringLiteral("scale_outer_rgba"), palette.scaleOuter, error)
        && readColor(object, QStringLiteral("scale_separator_rgba"), palette.scaleSeparator, error)
        && readColor(object, QStringLiteral("scale_calibration_rgba"), palette.scaleCalibration, error)
        && readColor(object, QStringLiteral("scale_inner_rgba"), palette.scaleInner, error)
        && readColor(object, QStringLiteral("major_tick_rgba"), palette.majorTick, error)
        && readColor(object, QStringLiteral("minor_tick_rgba"), palette.minorTick, error)
        && readColor(object, QStringLiteral("text_rgba"), palette.text, error)
        && readColor(object, QStringLiteral("secondary_text_rgba"), palette.secondaryText, error)
        && readColor(object, QStringLiteral("swr_guide_rgba"), palette.swrGuide, error)
        && readColor(object, QStringLiteral("swr_label_rgba"), palette.swrLabel, error)
        && readColor(object, QStringLiteral("needle_rgba"), palette.needle, error)
        && readColor(object, QStringLiteral("needle_edge_rgba"), palette.needleEdge, error)
        && readColor(object, QStringLiteral("needle_highlight_rgba"), palette.needleHighlight, error)
        && readColor(object, QStringLiteral("needle_shadow_rgba"), palette.needleShadow, error)
        && readColor(object, QStringLiteral("needle_soft_shadow_rgba"), palette.needleSoftShadow, error)
        && readColor(object, QStringLiteral("mask_fill_rgba"), palette.maskFill, error)
        && readColor(object, QStringLiteral("mask_edge_rgba"), palette.maskEdge, error)
        && readColor(object, QStringLiteral("mask_text_rgba"), palette.maskText, error);
}

const QImage& paperGrainTexture()
{
    static const QImage texture = []() {
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
            QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const quint32 sample = nextNoise();
                const int fine = static_cast<int>((sample >> 26U) & 0x3fU) - 32;
                horizontalFibre = (7 * horizontalFibre + fine) / 8;
                const int fleck = (sample & 0xffU) < 6U
                    ? (((sample >> 8U) & 1U) == 0U ? -34 : 34)
                    : 0;
                const int level = std::clamp(
                    128 + rowBias + fine + 2 * horizontalFibre + fleck, 58, 198);
                line[x] = qRgb(level, level, level);
            }
        }
        return image;
    }();
    return texture;
}

void drawPaperGrain(QPainter& painter, const QRectF& face, double opacity)
{
    if (opacity <= 0.0) {
        return;
    }
    painter.save();
    painter.setOpacity(opacity);
    painter.setCompositionMode(QPainter::CompositionMode_Overlay);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(face, paperGrainTexture());
    painter.restore();
}

bool validStop(double stop)
{
    return std::isfinite(stop) && stop > 0.0 && stop < 1.0;
}

} // namespace

QString analogMeterFaceThemeId(AnalogMeterFaceTheme theme)
{
    switch (theme) {
    case AnalogMeterFaceTheme::AetherDefault:
        return QStringLiteral("aether-default");
    case AnalogMeterFaceTheme::ClassicWarm:
        return QStringLiteral("classic-warm");
    case AnalogMeterFaceTheme::DarkRoomUplight:
        return QStringLiteral("dark-room-uplight");
    case AnalogMeterFaceTheme::GraphiteDark:
        return QStringLiteral("graphite-dark");
    }
    return QStringLiteral("aether-default");
}

AnalogMeterFaceTheme analogMeterFaceThemeFromId(
    const QString& id, AnalogMeterFaceTheme fallback)
{
    if (id == QStringLiteral("aether-default")) {
        return AnalogMeterFaceTheme::AetherDefault;
    }
    if (id == QStringLiteral("classic-warm")) {
        return AnalogMeterFaceTheme::ClassicWarm;
    }
    if (id == QStringLiteral("dark-room-uplight")) {
        return AnalogMeterFaceTheme::DarkRoomUplight;
    }
    if (id == QStringLiteral("graphite-dark")) {
        return AnalogMeterFaceTheme::GraphiteDark;
    }
    return fallback;
}

AnalogMeterFaceThemeCatalog AnalogMeterFaceThemeCatalog::loadResource(QString* error)
{
    QFile resource(QStringLiteral(":/meterfaces/analog-meter-themes-v1.json"));
    if (!resource.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot open shared analog meter theme resource");
        }
        return {};
    }
    return load(resource, error);
}

AnalogMeterFaceThemeCatalog AnalogMeterFaceThemeCatalog::load(
    QIODevice& device, QString* error)
{
    if (error) {
        error->clear();
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(device.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("shared analog meter theme root is not an object");
        }
        return {};
    }

    const QJsonObject root = document.object();
    AnalogMeterFaceThemeCatalog catalog = fallback();
    QString readError;
    if (!root.value(QStringLiteral("format_version")).isDouble()
        || root.value(QStringLiteral("format_version")).toInt() != kFormatVersion) {
        readError = QStringLiteral("unsupported shared analog meter theme version");
    }
    catalog.formatVersion = root.value(QStringLiteral("format_version")).toInt();
    if (readError.isEmpty()
        && !readRect(root, QStringLiteral("reference_face"), catalog.referenceFace, readError)) {
    }

    const QJsonObject mask = root.value(QStringLiteral("lower_mask")).toObject();
    if (readError.isEmpty() && mask.isEmpty()) {
        readError = QStringLiteral("lower_mask must be an object");
    }
    if (readError.isEmpty()) {
        const QJsonValue boundaryValue = mask.value(QStringLiteral("boundary_normalized"));
        if (!boundaryValue.isArray()) {
            readError = QStringLiteral("lower mask boundary must be an array");
        } else {
            QVector<QPointF> boundary;
            for (const QJsonValue& pointValue : boundaryValue.toArray()) {
                if (!pointValue.isArray()) {
                    readError = QStringLiteral("lower mask point must be a two-number array");
                    break;
                }
                const QJsonArray point = pointValue.toArray();
                if (point.size() != 2 || !point[0].isDouble() || !point[1].isDouble()
                    || !std::isfinite(point[0].toDouble())
                    || !std::isfinite(point[1].toDouble())) {
                    readError = QStringLiteral("lower mask point must be a two-number array");
                    break;
                }
                boundary.push_back(QPointF(point[0].toDouble(), point[1].toDouble()));
            }
            if (readError.isEmpty()) {
                catalog.normalizedMaskBoundary = boundary;
            }
        }
    }
    if (readError.isEmpty()) {
        readFiniteDouble(mask, QStringLiteral("bottom_normalized"),
                         catalog.normalizedMaskBottom, readError);
    }

    const QJsonObject classic = root.value(QStringLiteral("classic_gradient")).toObject();
    if (readError.isEmpty() && classic.isEmpty()) {
        readError = QStringLiteral("classic_gradient must be an object");
    }
    if (readError.isEmpty()) {
        FaceGradient& gradient = catalog.classicGradient;
        readColor(classic, QStringLiteral("top_rgba"), gradient.top, readError)
            && readColor(classic, QStringLiteral("middle_rgba"), gradient.middle, readError)
            && readColor(classic, QStringLiteral("bottom_rgba"), gradient.bottom, readError)
            && readFiniteDouble(classic, QStringLiteral("middle_stop"), gradient.middleStop, readError)
            && readPoint(classic, QStringLiteral("glow_center"), gradient.glowCenter, readError)
            && readFiniteDouble(classic, QStringLiteral("glow_radius"), gradient.glowRadius, readError)
            && readColor(classic, QStringLiteral("glow_inner_rgba"), gradient.glowInner, readError)
            && readColor(classic, QStringLiteral("glow_outer_rgba"), gradient.glowOuter, readError)
            && readPoint(classic, QStringLiteral("vignette_center"), gradient.vignetteCenter, readError)
            && readFiniteDouble(classic, QStringLiteral("vignette_radius"), gradient.vignetteRadius, readError)
            && readFiniteDouble(classic, QStringLiteral("vignette_clear_stop"), gradient.vignetteClearStop, readError)
            && readColor(classic, QStringLiteral("vignette_edge_rgba"), gradient.vignetteEdge, readError)
            && readFiniteDouble(classic, QStringLiteral("paper_grain_opacity"), gradient.paperGrainOpacity, readError);
    }

    const QJsonObject uplight = root.value(QStringLiteral("uplight_gradient")).toObject();
    if (readError.isEmpty() && uplight.isEmpty()) {
        readError = QStringLiteral("uplight_gradient must be an object");
    }
    if (readError.isEmpty()) {
        UplightGradient& gradient = catalog.uplightGradient;
        readColor(uplight, QStringLiteral("top_rgba"), gradient.top, readError)
            && readColor(uplight, QStringLiteral("middle_rgba"), gradient.middle, readError)
            && readColor(uplight, QStringLiteral("bottom_rgba"), gradient.bottom, readError)
            && readFiniteDouble(uplight, QStringLiteral("middle_stop"), gradient.middleStop, readError)
            && readPoint(uplight, QStringLiteral("halo_center"), gradient.haloCenter, readError)
            && readFiniteDouble(uplight, QStringLiteral("halo_radius"), gradient.haloRadius, readError)
            && readColor(uplight, QStringLiteral("halo_inner_rgba"), gradient.haloInner, readError)
            && readColor(uplight, QStringLiteral("halo_middle_rgba"), gradient.haloMiddle, readError)
            && readFiniteDouble(uplight, QStringLiteral("halo_middle_stop"), gradient.haloMiddleStop, readError)
            && readColor(uplight, QStringLiteral("halo_shoulder_rgba"), gradient.haloShoulder, readError)
            && readFiniteDouble(uplight, QStringLiteral("halo_shoulder_stop"), gradient.haloShoulderStop, readError)
            && readColor(uplight, QStringLiteral("halo_outer_rgba"), gradient.haloOuter, readError)
            && readPoint(uplight, QStringLiteral("hotspot_center"), gradient.hotspotCenter, readError)
            && readFiniteDouble(uplight, QStringLiteral("hotspot_radius"), gradient.hotspotRadius, readError)
            && readColor(uplight, QStringLiteral("hotspot_inner_rgba"), gradient.hotspotInner, readError)
            && readColor(uplight, QStringLiteral("hotspot_middle_rgba"), gradient.hotspotMiddle, readError)
            && readFiniteDouble(uplight, QStringLiteral("hotspot_middle_stop"), gradient.hotspotMiddleStop, readError)
            && readColor(uplight, QStringLiteral("hotspot_outer_rgba"), gradient.hotspotOuter, readError)
            && readPoint(uplight, QStringLiteral("bloom_center"), gradient.bloomCenter, readError)
            && readFiniteDouble(uplight, QStringLiteral("bloom_radius"), gradient.bloomRadius, readError)
            && readColor(uplight, QStringLiteral("bloom_inner_rgba"), gradient.bloomInner, readError)
            && readColor(uplight, QStringLiteral("bloom_middle_rgba"), gradient.bloomMiddle, readError)
            && readFiniteDouble(uplight, QStringLiteral("bloom_middle_stop"), gradient.bloomMiddleStop, readError)
            && readColor(uplight, QStringLiteral("bloom_outer_rgba"), gradient.bloomOuter, readError)
            && readPoint(uplight, QStringLiteral("vignette_center"), gradient.vignetteCenter, readError)
            && readFiniteDouble(uplight, QStringLiteral("vignette_radius"), gradient.vignetteRadius, readError)
            && readFiniteDouble(uplight, QStringLiteral("vignette_clear_stop"), gradient.vignetteClearStop, readError)
            && readColor(uplight, QStringLiteral("vignette_edge_rgba"), gradient.vignetteEdge, readError)
            && readFiniteDouble(uplight, QStringLiteral("paper_grain_opacity"), gradient.paperGrainOpacity, readError);
    }

    const QJsonObject dark = root.value(QStringLiteral("dark_gradient")).toObject();
    if (readError.isEmpty() && dark.isEmpty()) {
        readError = QStringLiteral("dark_gradient must be an object");
    }
    if (readError.isEmpty()) {
        DarkGradient& gradient = catalog.darkGradient;
        readColor(dark, QStringLiteral("top_rgba"), gradient.top, readError)
            && readColor(dark, QStringLiteral("middle_rgba"), gradient.middle, readError)
            && readColor(dark, QStringLiteral("bottom_rgba"), gradient.bottom, readError)
            && readFiniteDouble(dark, QStringLiteral("middle_stop"), gradient.middleStop, readError)
            && readPoint(dark, QStringLiteral("ambient_center"), gradient.ambientCenter, readError)
            && readFiniteDouble(dark, QStringLiteral("ambient_radius"), gradient.ambientRadius, readError)
            && readColor(dark, QStringLiteral("ambient_inner_rgba"), gradient.ambientInner, readError)
            && readColor(dark, QStringLiteral("ambient_outer_rgba"), gradient.ambientOuter, readError)
            && readPoint(dark, QStringLiteral("glow_center"), gradient.glowCenter, readError)
            && readFiniteDouble(dark, QStringLiteral("glow_radius"), gradient.glowRadius, readError)
            && readColor(dark, QStringLiteral("glow_inner_rgba"), gradient.glowInner, readError)
            && readColor(dark, QStringLiteral("glow_middle_rgba"), gradient.glowMiddle, readError)
            && readFiniteDouble(dark, QStringLiteral("glow_middle_stop"), gradient.glowMiddleStop, readError)
            && readColor(dark, QStringLiteral("glow_outer_rgba"), gradient.glowOuter, readError)
            && readPoint(dark, QStringLiteral("vignette_center"), gradient.vignetteCenter, readError)
            && readFiniteDouble(dark, QStringLiteral("vignette_radius"), gradient.vignetteRadius, readError)
            && readFiniteDouble(dark, QStringLiteral("vignette_clear_stop"), gradient.vignetteClearStop, readError)
            && readColor(dark, QStringLiteral("vignette_edge_rgba"), gradient.vignetteEdge, readError)
            && readFiniteDouble(dark, QStringLiteral("paper_grain_opacity"), gradient.paperGrainOpacity, readError);
    }

    const QJsonObject palettes = root.value(QStringLiteral("palettes")).toObject();
    if (readError.isEmpty() && palettes.isEmpty()) {
        readError = QStringLiteral("palettes must be an object");
    }
    if (readError.isEmpty()) {
        const QJsonObject classicPalette = palettes.value(QStringLiteral("classic-warm")).toObject();
        const QJsonObject uplightPalette = palettes.value(QStringLiteral("dark-room-uplight")).toObject();
        const QJsonObject darkPalette = palettes.value(QStringLiteral("graphite-dark")).toObject();
        if (classicPalette.isEmpty() || uplightPalette.isEmpty() || darkPalette.isEmpty()) {
            readError = QStringLiteral("all physical meter palettes are required");
        } else {
            readPalette(classicPalette, catalog.classicPalette, readError)
                && readPalette(uplightPalette, catalog.uplightPalette, readError)
                && readPalette(darkPalette, catalog.darkPalette, readError);
        }
    }

    QString validationError;
    if (readError.isEmpty() && !catalog.isValid(&validationError)) {
        readError = validationError;
    }
    if (!readError.isEmpty()) {
        if (error) {
            *error = readError;
        }
        return {};
    }
    return catalog;
}

AnalogMeterFaceThemeCatalog AnalogMeterFaceThemeCatalog::fallback()
{
    AnalogMeterFaceThemeCatalog catalog;
    catalog.formatVersion = kFormatVersion;
    catalog.referenceFace = QRectF(26.0, 26.0, 1448.0, 948.0);
    catalog.normalizedMaskBoundary = {
        {0.0179558011, 0.9261603376}, {0.1546961326, 0.9261603376},
        {0.3273480663, 0.9219409283}, {0.4309392265, 0.9008438819},
        {0.5, 0.8924050633},          {0.5690607735, 0.9008438819},
        {0.6726519337, 0.9219409283}, {0.8453038674, 0.9261603376},
        {0.9820441989, 0.9261603376}};
    catalog.normalizedMaskBottom = 1.0;

    catalog.classicGradient = {
        {232, 216, 183}, {250, 242, 221}, {222, 205, 172}, 0.50,
        {750.0, 400.0}, 900.0, {255, 250, 235, 205}, {255, 250, 235, 0},
        {750.0, 440.0}, 930.0, 0.62, {106, 97, 76, 65}, 0.0};
    catalog.uplightGradient = {
        {142, 110, 73}, {174, 126, 70}, {169, 119, 65}, 0.60,
        {750.0, 920.0}, 930.0, {255, 181, 83, 180}, {242, 160, 72, 170},
        0.52, {218, 143, 68, 40}, 0.72, {211, 137, 69, 0},
        {750.0, 930.0}, 470.0, {255, 245, 110, 220}, {255, 195, 65, 145},
        0.48, {255, 146, 44, 0}, {750.0, 940.0}, 330.0,
        {255, 246, 95, 225}, {255, 215, 65, 90}, 0.52, {255, 220, 130, 0},
        {750.0, 650.0}, 890.0, 0.34, {9, 10, 12, 120}, 0.100};
    catalog.darkGradient = {
        {20, 22, 23}, {27, 28, 28}, {25, 24, 23}, 0.58,
        {750.0, 410.0}, 900.0, {68, 66, 61, 55}, {40, 40, 38, 0},
        {750.0, 940.0}, 720.0, {190, 92, 35, 100}, {112, 59, 34, 38},
        0.58, {80, 43, 30, 0}, {750.0, 500.0}, 940.0, 0.42,
        {0, 0, 0, 120}, 0.220};

    const Palette physicalPalette{
        {224, 212, 187, 225}, {30, 36, 38}, {246, 237, 216},
        {126, 62, 54, 235}, {48, 65, 91}, {35, 42, 46},
        {55, 72, 91, 230}, {35, 42, 46}, {54, 61, 66},
        {161, 74, 58, 230}, {45, 45, 42}, {18, 22, 25},
        {2, 4, 5, 210}, {132, 132, 124, 175}, {0, 0, 0, 80},
        {0, 0, 0, 28}, {34, 40, 47}, {87, 99, 110}, {234, 238, 241}};
    catalog.classicPalette = physicalPalette;
    catalog.uplightPalette = physicalPalette;
    catalog.uplightPalette.scaleSeparator = QColor(117, 75, 48, 150);
    catalog.darkPalette = {
        {44, 43, 40, 225}, {202, 188, 159}, {126, 95, 73, 225},
        {119, 71, 59, 235}, {59, 84, 105}, {205, 191, 162},
        {151, 139, 117, 230}, {205, 190, 158}, {179, 162, 132},
        {139, 77, 58, 225}, {205, 190, 158}, {202, 197, 183},
        {82, 78, 69, 220}, {240, 234, 216}, {0, 0, 0, 105},
        {0, 0, 0, 36}, {20, 23, 26}, {64, 69, 74}, {219, 207, 181}};
    return catalog;
}

bool AnalogMeterFaceThemeCatalog::isValid(QString* error) const
{
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (error) {
        error->clear();
    }
    if (formatVersion != kFormatVersion) {
        return fail(QStringLiteral("unsupported shared analog meter theme version"));
    }
    if (!(referenceFace.width() > 0.0) || !(referenceFace.height() > 0.0)) {
        return fail(QStringLiteral("shared analog meter reference face is invalid"));
    }
    if (normalizedMaskBoundary.size() != 9
        || !(normalizedMaskBottom > 0.0 && normalizedMaskBottom <= 1.0)) {
        return fail(QStringLiteral("shared analog meter lower mask is invalid"));
    }
    double previousX = -1.0;
    for (const QPointF& point : normalizedMaskBoundary) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())
            || point.x() < 0.0 || point.x() > 1.0
            || point.y() < 0.0 || point.y() > normalizedMaskBottom
            || point.x() <= previousX) {
            return fail(QStringLiteral("shared analog meter lower mask is invalid"));
        }
        previousX = point.x();
    }
    for (int index = 0; index < normalizedMaskBoundary.size() / 2; ++index) {
        const QPointF left = normalizedMaskBoundary[index];
        const QPointF right = normalizedMaskBoundary[normalizedMaskBoundary.size() - 1 - index];
        if (std::abs((left.x() + right.x()) - 1.0) > 1e-6
            || std::abs(left.y() - right.y()) > 1e-6) {
            return fail(QStringLiteral("shared analog meter lower mask is not symmetric"));
        }
    }
    const QPointF center = normalizedMaskBoundary.at(normalizedMaskBoundary.size() / 2);
    if (std::abs(center.x() - 0.5) > 1e-6) {
        return fail(QStringLiteral("shared analog meter lower mask is not symmetric"));
    }
    if (!validStop(classicGradient.middleStop)
        || !validStop(classicGradient.vignetteClearStop)
        || !(classicGradient.glowRadius > 0.0)
        || !(classicGradient.vignetteRadius > 0.0)
        || classicGradient.paperGrainOpacity < 0.0
        || classicGradient.paperGrainOpacity > 0.30) {
        return fail(QStringLiteral("classic analog meter material is invalid"));
    }
    if (!validStop(uplightGradient.middleStop)
        || !validStop(uplightGradient.haloMiddleStop)
        || !validStop(uplightGradient.haloShoulderStop)
        || !(uplightGradient.haloMiddleStop < uplightGradient.haloShoulderStop)
        || !validStop(uplightGradient.hotspotMiddleStop)
        || !validStop(uplightGradient.bloomMiddleStop)
        || !validStop(uplightGradient.vignetteClearStop)
        || !(uplightGradient.haloRadius > 0.0)
        || !(uplightGradient.hotspotRadius > 0.0)
        || !(uplightGradient.bloomRadius > 0.0)
        || !(uplightGradient.vignetteRadius > 0.0)
        || uplightGradient.paperGrainOpacity < 0.0
        || uplightGradient.paperGrainOpacity > 0.30) {
        return fail(QStringLiteral("uplight analog meter material is invalid"));
    }
    if (!validStop(darkGradient.middleStop)
        || !validStop(darkGradient.glowMiddleStop)
        || !validStop(darkGradient.vignetteClearStop)
        || !(darkGradient.ambientRadius > 0.0)
        || !(darkGradient.glowRadius > 0.0)
        || !(darkGradient.vignetteRadius > 0.0)
        || darkGradient.paperGrainOpacity < 0.0
        || darkGradient.paperGrainOpacity > 0.30) {
        return fail(QStringLiteral("graphite analog meter material is invalid"));
    }
    return true;
}

const AnalogMeterFaceThemeCatalog::Palette& AnalogMeterFaceThemeCatalog::palette(
    AnalogMeterFaceTheme theme) const
{
    switch (theme) {
    case AnalogMeterFaceTheme::DarkRoomUplight:
        return uplightPalette;
    case AnalogMeterFaceTheme::GraphiteDark:
        return darkPalette;
    case AnalogMeterFaceTheme::AetherDefault:
    case AnalogMeterFaceTheme::ClassicWarm:
        return classicPalette;
    }
    return classicPalette;
}

void AnalogMeterFaceThemeCatalog::drawBackground(
    QPainter& painter, const QRectF& face, AnalogMeterFaceTheme theme) const
{
    if (theme == AnalogMeterFaceTheme::AetherDefault) {
        return;
    }

    painter.save();
    painter.setClipRect(face, Qt::IntersectClip);
    painter.translate(face.left(), face.top());
    painter.scale(face.width() / referenceFace.width(),
                  face.height() / referenceFace.height());
    painter.translate(-referenceFace.left(), -referenceFace.top());
    const QRectF materialFace = referenceFace;

    if (theme == AnalogMeterFaceTheme::GraphiteDark) {
        const DarkGradient& dark = darkGradient;
        QLinearGradient card(materialFace.topLeft(), materialFace.bottomLeft());
        card.setColorAt(0.0, dark.top);
        card.setColorAt(dark.middleStop, dark.middle);
        card.setColorAt(1.0, dark.bottom);
        painter.fillRect(materialFace, card);

        QRadialGradient ambient(dark.ambientCenter, dark.ambientRadius);
        ambient.setColorAt(0.0, dark.ambientInner);
        ambient.setColorAt(1.0, dark.ambientOuter);
        painter.fillRect(materialFace, ambient);

        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        QRadialGradient glow(dark.glowCenter, dark.glowRadius);
        glow.setColorAt(0.0, dark.glowInner);
        glow.setColorAt(dark.glowMiddleStop, dark.glowMiddle);
        glow.setColorAt(1.0, dark.glowOuter);
        painter.fillRect(materialFace, glow);
        painter.restore();

        QColor clearEdge = dark.vignetteEdge;
        clearEdge.setAlpha(0);
        QRadialGradient vignette(dark.vignetteCenter, dark.vignetteRadius);
        vignette.setColorAt(0.0, clearEdge);
        vignette.setColorAt(dark.vignetteClearStop, clearEdge);
        vignette.setColorAt(1.0, dark.vignetteEdge);
        painter.fillRect(materialFace, vignette);
        drawPaperGrain(painter, materialFace, dark.paperGrainOpacity);
        painter.restore();
        return;
    }

    if (theme == AnalogMeterFaceTheme::DarkRoomUplight) {
        const UplightGradient& light = uplightGradient;
        QLinearGradient ambient(materialFace.topLeft(), materialFace.bottomLeft());
        ambient.setColorAt(0.0, light.top);
        ambient.setColorAt(light.middleStop, light.middle);
        ambient.setColorAt(1.0, light.bottom);
        painter.fillRect(materialFace, ambient);

        QRadialGradient halo(light.haloCenter, light.haloRadius);
        halo.setColorAt(0.0, light.haloInner);
        halo.setColorAt(light.haloMiddleStop, light.haloMiddle);
        halo.setColorAt(light.haloShoulderStop, light.haloShoulder);
        halo.setColorAt(1.0, light.haloOuter);
        painter.fillRect(materialFace, halo);

        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        QRadialGradient hotspot(light.hotspotCenter, light.hotspotRadius);
        hotspot.setColorAt(0.0, light.hotspotInner);
        hotspot.setColorAt(light.hotspotMiddleStop, light.hotspotMiddle);
        hotspot.setColorAt(1.0, light.hotspotOuter);
        painter.fillRect(materialFace, hotspot);
        QRadialGradient bloom(light.bloomCenter, light.bloomRadius);
        bloom.setColorAt(0.0, light.bloomInner);
        bloom.setColorAt(light.bloomMiddleStop, light.bloomMiddle);
        bloom.setColorAt(1.0, light.bloomOuter);
        painter.fillRect(materialFace, bloom);
        painter.restore();

        QColor clearEdge = light.vignetteEdge;
        clearEdge.setAlpha(0);
        QRadialGradient vignette(light.vignetteCenter, light.vignetteRadius);
        vignette.setColorAt(0.0, clearEdge);
        vignette.setColorAt(light.vignetteClearStop, clearEdge);
        vignette.setColorAt(1.0, light.vignetteEdge);
        painter.fillRect(materialFace, vignette);
        drawPaperGrain(painter, materialFace, light.paperGrainOpacity);
        painter.restore();
        return;
    }

    const FaceGradient& material = classicGradient;
    QLinearGradient base(materialFace.topLeft(), materialFace.bottomLeft());
    base.setColorAt(0.0, material.top);
    base.setColorAt(material.middleStop, material.middle);
    base.setColorAt(1.0, material.bottom);
    painter.fillRect(materialFace, base);
    QRadialGradient glow(material.glowCenter, material.glowRadius);
    glow.setColorAt(0.0, material.glowInner);
    glow.setColorAt(1.0, material.glowOuter);
    painter.fillRect(materialFace, glow);
    QColor clearEdge = material.vignetteEdge;
    clearEdge.setAlpha(0);
    QRadialGradient vignette(material.vignetteCenter, material.vignetteRadius);
    vignette.setColorAt(0.0, clearEdge);
    vignette.setColorAt(material.vignetteClearStop, clearEdge);
    vignette.setColorAt(1.0, material.vignetteEdge);
    painter.fillRect(materialFace, vignette);
    drawPaperGrain(painter, materialFace, material.paperGrainOpacity);
    painter.restore();
}

QVector<QPointF> AnalogMeterFaceThemeCatalog::lowerMaskBoundary(
    const QRectF& face) const
{
    QVector<QPointF> boundary;
    boundary.reserve(normalizedMaskBoundary.size());
    for (const QPointF& normalized : normalizedMaskBoundary) {
        boundary.push_back(QPointF(face.left() + normalized.x() * face.width(),
                                   face.top() + normalized.y() * face.height()));
    }
    return boundary;
}

QPainterPath AnalogMeterFaceThemeCatalog::lowerMaskPath(const QRectF& face) const
{
    const QVector<QPointF> boundary = lowerMaskBoundary(face);
    QPainterPath path;
    if (boundary.isEmpty()) {
        return path;
    }
    const double bottom = face.top() + normalizedMaskBottom * face.height();
    path.moveTo(boundary.first().x(), bottom);
    for (const QPointF& point : boundary) {
        path.lineTo(point);
    }
    path.lineTo(boundary.last().x(), bottom);
    path.closeSubpath();
    return path;
}

} // namespace AetherSDR
