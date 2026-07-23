#include "SMeterGeometry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR {

namespace {

bool finite(double value)
{
    return std::isfinite(value);
}

bool positive(double value)
{
    return finite(value) && value > 0.0;
}

bool nonNegative(double value)
{
    return finite(value) && value >= 0.0;
}

bool nearlyEqual(double first, double second, double tolerance = 1.0e-9)
{
    return finite(first) && finite(second) && std::abs(first - second) <= tolerance;
}

bool parseFailure(QString& error, const QString& path, const QString& requirement)
{
    error = QStringLiteral("s-meter-v1.json: %1 must be %2").arg(path, requirement);
    return false;
}

QString childPath(const QString& parent, const QString& key)
{
    return parent.isEmpty() ? key : parent + QLatin1Char('.') + key;
}

bool readObject(const QJsonObject& object, const QString& key, const QString& parentPath,
                QJsonObject& output, QString& error)
{
    const QString path = childPath(parentPath, key);
    const QJsonValue value = object.value(key);
    if (!value.isObject()) {
        return parseFailure(error, path, QStringLiteral("an object"));
    }
    output = value.toObject();
    return true;
}

bool readNumberValue(const QJsonValue& value, const QString& path, double& output,
                     QString& error)
{
    if (!value.isDouble() || !finite(value.toDouble())) {
        return parseFailure(error, path, QStringLiteral("a finite number"));
    }
    output = value.toDouble();
    return true;
}

bool readNumber(const QJsonObject& object, const QString& key, const QString& parentPath,
                double& output, QString& error)
{
    return readNumberValue(object.value(key), childPath(parentPath, key), output, error);
}

bool readIntValue(const QJsonValue& value, const QString& path, int& output, QString& error)
{
    double number = 0.0;
    if (!readNumberValue(value, path, number, error)) {
        return false;
    }
    if (std::floor(number) != number || number < std::numeric_limits<int>::min()
        || number > std::numeric_limits<int>::max()) {
        return parseFailure(error, path, QStringLiteral("an integer"));
    }
    output = static_cast<int>(number);
    return true;
}

bool readInt(const QJsonObject& object, const QString& key, const QString& parentPath,
             int& output, QString& error)
{
    return readIntValue(object.value(key), childPath(parentPath, key), output, error);
}

bool readBool(const QJsonObject& object, const QString& key, const QString& parentPath,
              bool& output, QString& error)
{
    const QString path = childPath(parentPath, key);
    const QJsonValue value = object.value(key);
    if (!value.isBool()) {
        return parseFailure(error, path, QStringLiteral("a boolean"));
    }
    output = value.toBool();
    return true;
}

bool readString(const QJsonObject& object, const QString& key, const QString& parentPath,
                QString& output, QString& error)
{
    const QString path = childPath(parentPath, key);
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
        return parseFailure(error, path, QStringLiteral("a string"));
    }
    output = value.toString();
    return true;
}

bool readSize(const QJsonObject& object, const QString& key, const QString& parentPath,
              QSize& output, QString& error)
{
    const QString path = childPath(parentPath, key);
    const QJsonValue value = object.value(key);
    if (!value.isArray()) {
        return parseFailure(error, path, QStringLiteral("a two-integer array"));
    }
    const QJsonArray values = value.toArray();
    if (values.size() != 2) {
        return parseFailure(error, path, QStringLiteral("a two-integer array"));
    }
    int width = 0;
    int height = 0;
    if (!readIntValue(values.at(0), path + QStringLiteral("[0]"), width, error)
        || !readIntValue(values.at(1), path + QStringLiteral("[1]"), height, error)) {
        return false;
    }
    output = QSize(width, height);
    return true;
}

bool readPoint(const QJsonObject& object, const QString& key, const QString& parentPath,
               QPointF& output, QString& error)
{
    const QString path = childPath(parentPath, key);
    const QJsonValue value = object.value(key);
    if (!value.isArray()) {
        return parseFailure(error, path, QStringLiteral("a two-number array"));
    }
    const QJsonArray values = value.toArray();
    if (values.size() != 2) {
        return parseFailure(error, path, QStringLiteral("a two-number array"));
    }
    double x = 0.0;
    double y = 0.0;
    if (!readNumberValue(values.at(0), path + QStringLiteral("[0]"), x, error)
        || !readNumberValue(values.at(1), path + QStringLiteral("[1]"), y, error)) {
        return false;
    }
    output = QPointF(x, y);
    return true;
}

bool readTicks(const QJsonObject& object, const QString& key, const QString& parentPath,
               QVector<SMeterGeometry::Tick>& output, QString& error)
{
    const QString path = childPath(parentPath, key);
    const QJsonValue value = object.value(key);
    if (!value.isArray()) {
        return parseFailure(error, path, QStringLiteral("an array"));
    }

    const QJsonArray values = value.toArray();
    output.clear();
    output.reserve(values.size());
    for (qsizetype index = 0; index < values.size(); ++index) {
        const QString entryPath = path + QStringLiteral("[%1]").arg(index);
        if (!values.at(index).isObject()) {
            return parseFailure(error, entryPath, QStringLiteral("an object"));
        }
        const QJsonObject tickObject = values.at(index).toObject();
        SMeterGeometry::Tick tick;
        if (!readNumber(tickObject, QStringLiteral("value"), entryPath, tick.value, error)
            || !readString(tickObject, QStringLiteral("label"), entryPath, tick.label, error)) {
            return false;
        }
        output.push_back(tick);
    }
    return true;
}

bool readStaticScale(const QJsonObject& root, const QString& key,
                     SMeterGeometry::StaticScale& scale, QString& error)
{
    QJsonObject object;
    if (!readObject(root, key, QString(), object, error)) {
        return false;
    }
    return readNumber(object, QStringLiteral("minimum"), key, scale.minimum, error)
        && readNumber(object, QStringLiteral("maximum"), key, scale.maximum, error)
        && readBool(object, QStringLiteral("has_warning"), key, scale.hasWarning, error)
        && readNumber(object, QStringLiteral("warning_start"), key, scale.warningStart, error)
        && readTicks(object, QStringLiteral("ticks"), key, scale.ticks, error);
}

SMeterGeometry rejectedGeometry(QString* error, const QString& message)
{
    if (error) {
        *error = message;
    }
    return {};
}

bool validTicks(const QVector<SMeterGeometry::Tick>& ticks, double minimum, double maximum)
{
    if (ticks.isEmpty()) {
        return false;
    }
    double previous = -std::numeric_limits<double>::infinity();
    for (const SMeterGeometry::Tick& tick : ticks) {
        if (!finite(tick.value) || tick.value < minimum || tick.value > maximum
            || tick.value <= previous || tick.label.isEmpty()) {
            return false;
        }
        previous = tick.value;
    }
    return true;
}

} // namespace

SMeterGeometry SMeterGeometry::loadResource(QString* error)
{
    QFile file(QStringLiteral(":/meterfaces/s-meter-v1.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot open :/meterfaces/s-meter-v1.json");
        }
        return {};
    }
    return load(file, error);
}

SMeterGeometry SMeterGeometry::load(QIODevice& device, QString* error)
{
    if (error) {
        error->clear();
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(device.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return rejectedGeometry(
            error, QStringLiteral("invalid JSON: %1").arg(parseError.errorString()));
    }

    const QJsonObject root = document.object();
    SMeterGeometry geometry;
    QString fieldError;
    QJsonObject sizing;
    QJsonObject arc;
    QJsonObject tickStyle;
    QJsonObject rxScale;
    QJsonObject needle;
    QJsonObject pivot;
    QJsonObject peakMarker;
    QJsonObject peakHold;
    QJsonObject readout;

    const bool fixedFieldsValid =
        readInt(root, QStringLiteral("format_version"), QString(), geometry.formatVersion,
                fieldError)
        && readInt(root, QStringLiteral("design_version"), QString(), geometry.designVersion,
                   fieldError)
        && readObject(root, QStringLiteral("sizing"), QString(), sizing, fieldError)
        && readSize(sizing, QStringLiteral("preferred"), QStringLiteral("sizing"),
                    geometry.sizing.preferred, fieldError)
        && readSize(sizing, QStringLiteral("minimum"), QStringLiteral("sizing"),
                    geometry.sizing.minimum, fieldError)
        && readNumber(sizing, QStringLiteral("minimum_aspect_ratio"),
                      QStringLiteral("sizing"), geometry.sizing.minimumAspectRatio,
                      fieldError)
        && readNumber(sizing, QStringLiteral("maximum_aspect_ratio"),
                      QStringLiteral("sizing"), geometry.sizing.maximumAspectRatio,
                      fieldError)
        && readObject(root, QStringLiteral("arc"), QString(), arc, fieldError)
        && readNumber(arc, QStringLiteral("center_x_width_factor"), QStringLiteral("arc"),
                      geometry.arc.centerXWidthFactor, fieldError)
        && readNumber(arc, QStringLiteral("radius_width_factor"), QStringLiteral("arc"),
                      geometry.arc.radiusWidthFactor, fieldError)
        && readNumber(arc, QStringLiteral("center_y_height_factor"), QStringLiteral("arc"),
                      geometry.arc.centerYHeightFactor, fieldError)
        && readNumber(arc, QStringLiteral("start_degrees"), QStringLiteral("arc"),
                      geometry.arc.startDegrees, fieldError)
        && readNumber(arc, QStringLiteral("end_degrees"), QStringLiteral("arc"),
                      geometry.arc.endDegrees, fieldError)
        && readNumber(arc, QStringLiteral("inner_gap_pixels"), QStringLiteral("arc"),
                      geometry.arc.innerGapPixels, fieldError)
        && readNumber(arc, QStringLiteral("line_width_pixels"), QStringLiteral("arc"),
                      geometry.arc.lineWidthPixels, fieldError)
        && readObject(root, QStringLiteral("tick_style"), QString(), tickStyle, fieldError)
        && readNumber(tickStyle, QStringLiteral("start_offset_pixels"),
                      QStringLiteral("tick_style"), geometry.tickStyle.startOffsetPixels,
                      fieldError)
        && readNumber(tickStyle, QStringLiteral("end_offset_pixels"),
                      QStringLiteral("tick_style"), geometry.tickStyle.endOffsetPixels,
                      fieldError)
        && readNumber(tickStyle, QStringLiteral("label_offset_pixels"),
                      QStringLiteral("tick_style"), geometry.tickStyle.labelOffsetPixels,
                      fieldError)
        && readNumber(tickStyle, QStringLiteral("line_width_pixels"),
                      QStringLiteral("tick_style"), geometry.tickStyle.lineWidthPixels,
                      fieldError)
        && readInt(tickStyle, QStringLiteral("font_minimum_pixels"),
                   QStringLiteral("tick_style"), geometry.tickStyle.fontMinimumPixels,
                   fieldError)
        && readNumber(tickStyle, QStringLiteral("font_height_factor"),
                      QStringLiteral("tick_style"), geometry.tickStyle.fontHeightFactor,
                      fieldError)
        && readBool(tickStyle, QStringLiteral("bold"), QStringLiteral("tick_style"),
                    geometry.tickStyle.bold, fieldError)
        && readObject(root, QStringLiteral("rx_scale"), QString(), rxScale, fieldError)
        && readNumber(rxScale, QStringLiteral("minimum_dbm"), QStringLiteral("rx_scale"),
                      geometry.rxScale.minimumDbm, fieldError)
        && readNumber(rxScale, QStringLiteral("s9_dbm"), QStringLiteral("rx_scale"),
                      geometry.rxScale.s9Dbm, fieldError)
        && readNumber(rxScale, QStringLiteral("maximum_dbm"), QStringLiteral("rx_scale"),
                      geometry.rxScale.maximumDbm, fieldError)
        && readNumber(rxScale, QStringLiteral("db_per_s_unit"), QStringLiteral("rx_scale"),
                      geometry.rxScale.dbPerSUnit, fieldError)
        && readNumber(rxScale, QStringLiteral("s9_fraction"), QStringLiteral("rx_scale"),
                      geometry.rxScale.s9Fraction, fieldError)
        && readTicks(rxScale, QStringLiteral("ticks"), QStringLiteral("rx_scale"),
                     geometry.rxScale.ticks, fieldError)
        && readStaticScale(root, QStringLiteral("swr_scale"), geometry.swrScale, fieldError)
        && readStaticScale(root, QStringLiteral("level_scale"), geometry.levelScale, fieldError)
        && readStaticScale(root, QStringLiteral("compression_scale"),
                           geometry.compressionScale, fieldError)
        && readObject(root, QStringLiteral("needle"), QString(), needle, fieldError)
        && readNumber(needle, QStringLiteral("pivot_y_below_widget_pixels"),
                      QStringLiteral("needle"), geometry.needle.pivotYBelowWidgetPixels,
                      fieldError)
        && readNumber(needle, QStringLiteral("tip_extension_pixels"),
                      QStringLiteral("needle"), geometry.needle.tipExtensionPixels,
                      fieldError)
        && readNumber(needle, QStringLiteral("line_width_pixels"), QStringLiteral("needle"),
                      geometry.needle.lineWidthPixels, fieldError)
        && readNumber(needle, QStringLiteral("shadow_width_pixels"),
                      QStringLiteral("needle"), geometry.needle.shadowWidthPixels, fieldError)
        && readPoint(needle, QStringLiteral("shadow_offset"), QStringLiteral("needle"),
                     geometry.needle.shadowOffset, fieldError)
        && readObject(root, QStringLiteral("pivot"), QString(), pivot, fieldError)
        && readNumber(pivot, QStringLiteral("minimum_radius_pixels"), QStringLiteral("pivot"),
                      geometry.pivot.minimumRadiusPixels, fieldError)
        && readNumber(pivot, QStringLiteral("radius_width_factor"), QStringLiteral("pivot"),
                      geometry.pivot.radiusWidthFactor, fieldError)
        && readNumber(pivot, QStringLiteral("glow_radius_factor"), QStringLiteral("pivot"),
                      geometry.pivot.glowRadiusFactor, fieldError)
        && readNumber(pivot, QStringLiteral("glow_middle_factor"), QStringLiteral("pivot"),
                      geometry.pivot.glowMiddleFactor, fieldError)
        && readInt(pivot, QStringLiteral("glow_center_alpha"), QStringLiteral("pivot"),
                   geometry.pivot.glowCenterAlpha, fieldError)
        && readInt(pivot, QStringLiteral("glow_middle_alpha"), QStringLiteral("pivot"),
                   geometry.pivot.glowMiddleAlpha, fieldError)
        && readNumber(pivot, QStringLiteral("rim_width_pixels"), QStringLiteral("pivot"),
                      geometry.pivot.rimWidthPixels, fieldError)
        && readObject(root, QStringLiteral("peak_marker"), QString(), peakMarker, fieldError)
        && readNumber(peakMarker, QStringLiteral("radius_inset_pixels"),
                      QStringLiteral("peak_marker"), geometry.peakMarker.radiusInsetPixels,
                      fieldError)
        && readNumber(peakMarker, QStringLiteral("length_pixels"),
                      QStringLiteral("peak_marker"), geometry.peakMarker.lengthPixels,
                      fieldError)
        && readNumber(peakMarker, QStringLiteral("half_width_pixels"),
                      QStringLiteral("peak_marker"), geometry.peakMarker.halfWidthPixels,
                      fieldError)
        && readNumber(peakMarker, QStringLiteral("minimum_lead_db"),
                      QStringLiteral("peak_marker"), geometry.peakMarker.minimumLeadDb,
                      fieldError)
        && readObject(root, QStringLiteral("peak_hold"), QString(), peakHold, fieldError)
        && readNumber(peakHold, QStringLiteral("inner_radius_offset_pixels"),
                      QStringLiteral("peak_hold"), geometry.peakHold.innerRadiusOffsetPixels,
                      fieldError)
        && readNumber(peakHold, QStringLiteral("outer_radius_offset_pixels"),
                      QStringLiteral("peak_hold"), geometry.peakHold.outerRadiusOffsetPixels,
                      fieldError)
        && readNumber(peakHold, QStringLiteral("line_width_pixels"),
                      QStringLiteral("peak_hold"), geometry.peakHold.lineWidthPixels,
                      fieldError)
        && readNumber(peakHold, QStringLiteral("visible_above_minimum_db"),
                      QStringLiteral("peak_hold"), geometry.peakHold.visibleAboveMinimumDb,
                      fieldError)
        && readObject(root, QStringLiteral("readout"), QString(), readout, fieldError)
        && readInt(readout, QStringLiteral("source_font_minimum_pixels"),
                   QStringLiteral("readout"), geometry.readout.sourceFontMinimumPixels,
                   fieldError)
        && readNumber(readout, QStringLiteral("source_font_height_divisor"),
                      QStringLiteral("readout"), geometry.readout.sourceFontHeightDivisor,
                      fieldError)
        && readInt(readout, QStringLiteral("value_font_minimum_pixels"),
                   QStringLiteral("readout"), geometry.readout.valueFontMinimumPixels,
                   fieldError)
        && readNumber(readout, QStringLiteral("value_font_height_divisor"),
                      QStringLiteral("readout"), geometry.readout.valueFontHeightDivisor,
                      fieldError)
        && readInt(readout, QStringLiteral("top_extra_pixels"), QStringLiteral("readout"),
                   geometry.readout.topExtraPixels, fieldError)
        && readInt(readout, QStringLiteral("side_margin_pixels"), QStringLiteral("readout"),
                   geometry.readout.sideMarginPixels, fieldError);
    if (!fixedFieldsValid) {
        return rejectedGeometry(error, fieldError);
    }

    const QJsonValue policiesValue = root.value(QStringLiteral("power_tick_policies"));
    if (!policiesValue.isArray()) {
        return rejectedGeometry(error,
                                QStringLiteral("s-meter-v1.json: power_tick_policies must be an array"));
    }
    const QJsonArray policies = policiesValue.toArray();
    geometry.powerTickPolicies.clear();
    geometry.powerTickPolicies.reserve(policies.size());
    for (qsizetype index = 0; index < policies.size(); ++index) {
        const QString path = QStringLiteral("power_tick_policies[%1]").arg(index);
        if (!policies.at(index).isObject()) {
            return rejectedGeometry(
                error, QStringLiteral("s-meter-v1.json: %1 must be an object").arg(path));
        }
        const QJsonObject policyObject = policies.at(index).toObject();
        PowerTickPolicy policy;
        if (!readNumber(policyObject, QStringLiteral("minimum_scale_watts"), path,
                        policy.minimumScaleWatts, fieldError)
            || !readInt(policyObject, QStringLiteral("tick_step_watts"), path,
                        policy.tickStepWatts, fieldError)
            || !readInt(policyObject, QStringLiteral("label_step_watts"), path,
                        policy.labelStepWatts, fieldError)) {
            return rejectedGeometry(error, fieldError);
        }
        geometry.powerTickPolicies.push_back(policy);
    }

    QString validationError;
    if (!geometry.isValid(&validationError)) {
        return rejectedGeometry(error, validationError);
    }
    return geometry;
}

SMeterGeometry SMeterGeometry::fallback()
{
    SMeterGeometry geometry;
    geometry.formatVersion = 1;
    geometry.designVersion = 5;
    geometry.rxScale.ticks = {{-121.0, QStringLiteral("1")},
                              {-109.0, QStringLiteral("3")},
                              {-97.0, QStringLiteral("5")},
                              {-85.0, QStringLiteral("7")},
                              {-73.0, QStringLiteral("9")},
                              {-53.0, QStringLiteral("+20")},
                              {-33.0, QStringLiteral("+40")}};
    geometry.swrScale =
        {1.0,
         3.0,
         2.5,
         true,
         {{1.0, QStringLiteral("1")},
          {1.5, QStringLiteral("1.5")},
          {2.0, QStringLiteral("2")},
          {2.5, QStringLiteral("2.5")},
          {3.0, QStringLiteral("3")}}};
    geometry.levelScale =
        {-40.0,
         5.0,
         0.0,
         true,
         {{-40.0, QStringLiteral("-40")},
          {-30.0, QStringLiteral("-30")},
          {-20.0, QStringLiteral("-20")},
          {-10.0, QStringLiteral("-10")},
          {0.0, QStringLiteral("0")}}};
    geometry.compressionScale =
        {0.0,
         25.0,
         0.0,
         false,
         {{0.0, QStringLiteral("0")},
          {5.0, QStringLiteral("-5")},
          {10.0, QStringLiteral("-10")},
          {15.0, QStringLiteral("-15")},
          {20.0, QStringLiteral("-20")},
          {25.0, QStringLiteral("-25")}}};
    geometry.powerTickPolicies = {{2000.0, 100, 500}, {600.0, 50, 100}, {0.0, 10, 40}};
    return geometry;
}

bool SMeterGeometry::isValid(QString* error) const
{
    if (error) {
        error->clear();
    }
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (formatVersion != 1 || designVersion <= 0) {
        return fail(QStringLiteral("unsupported standard S-meter geometry version"));
    }
    const double preferredAspectRatio =
        static_cast<double>(sizing.preferred.width())
        / static_cast<double>(std::max(sizing.preferred.height(), 1));
    if (sizing.preferred.width() <= 0 || sizing.preferred.height() <= 0
        || sizing.minimum.width() <= 0 || sizing.minimum.height() <= 0
        || sizing.preferred.width() < sizing.minimum.width()
        || sizing.preferred.height() < sizing.minimum.height()
        || !positive(sizing.minimumAspectRatio)
        || !positive(sizing.maximumAspectRatio)
        || sizing.minimumAspectRatio >= sizing.maximumAspectRatio
        || preferredAspectRatio < sizing.minimumAspectRatio
        || preferredAspectRatio > sizing.maximumAspectRatio) {
        return fail(QStringLiteral("standard S-meter sizing is invalid"));
    }
    if (!(arc.centerXWidthFactor > 0.0 && arc.centerXWidthFactor < 1.0)
        || !positive(arc.radiusWidthFactor) || !positive(arc.centerYHeightFactor)
        || !(arc.startDegrees > 0.0 && arc.startDegrees < arc.endDegrees
             && arc.endDegrees < 180.0)
        || !positive(arc.innerGapPixels) || !positive(arc.lineWidthPixels)) {
        return fail(QStringLiteral("standard S-meter arc is invalid"));
    }
    if (!nonNegative(tickStyle.startOffsetPixels) || !positive(tickStyle.endOffsetPixels)
        || tickStyle.endOffsetPixels <= tickStyle.startOffsetPixels
        || !positive(tickStyle.labelOffsetPixels)
        || tickStyle.labelOffsetPixels <= tickStyle.endOffsetPixels
        || !positive(tickStyle.lineWidthPixels) || tickStyle.fontMinimumPixels <= 0
        || !positive(tickStyle.fontHeightFactor)) {
        return fail(QStringLiteral("standard S-meter tick style is invalid"));
    }
    if (!finite(rxScale.minimumDbm) || !finite(rxScale.s9Dbm)
        || !finite(rxScale.maximumDbm) || rxScale.minimumDbm >= rxScale.s9Dbm
        || rxScale.s9Dbm >= rxScale.maximumDbm || !positive(rxScale.dbPerSUnit)
        || !(rxScale.s9Fraction > 0.0 && rxScale.s9Fraction < 1.0)
        || !nearlyEqual(rxScale.s9Dbm - rxScale.minimumDbm, 9.0 * rxScale.dbPerSUnit)
        || !nearlyEqual(rxScale.maximumDbm - rxScale.s9Dbm, 60.0)
        || !validTicks(rxScale.ticks, rxScale.minimumDbm, rxScale.maximumDbm)) {
        return fail(QStringLiteral("standard S-meter RX scale is invalid"));
    }
    const auto validStaticScale = [](const StaticScale& scale) {
        return finite(scale.minimum) && finite(scale.maximum) && scale.minimum < scale.maximum
            && (!scale.hasWarning
                || (finite(scale.warningStart) && scale.warningStart >= scale.minimum
                    && scale.warningStart <= scale.maximum))
            && validTicks(scale.ticks, scale.minimum, scale.maximum);
    };
    if (!validStaticScale(swrScale) || !validStaticScale(levelScale)
        || !validStaticScale(compressionScale)) {
        return fail(QStringLiteral("standard S-meter TX scale is invalid"));
    }
    if (powerTickPolicies.isEmpty()) {
        return fail(QStringLiteral("standard S-meter power tick policies are empty"));
    }
    double previousMinimum = std::numeric_limits<double>::infinity();
    for (const PowerTickPolicy& policy : powerTickPolicies) {
        if (!nonNegative(policy.minimumScaleWatts) || policy.minimumScaleWatts >= previousMinimum
            || policy.tickStepWatts <= 0 || policy.labelStepWatts <= 0
            || policy.labelStepWatts % policy.tickStepWatts != 0) {
            return fail(QStringLiteral("standard S-meter power tick policy is invalid"));
        }
        previousMinimum = policy.minimumScaleWatts;
    }
    if (powerTickPolicies.constLast().minimumScaleWatts != 0.0) {
        return fail(QStringLiteral("standard S-meter power tick policies lack a default"));
    }
    if (!nonNegative(needle.pivotYBelowWidgetPixels)
        || !nonNegative(needle.tipExtensionPixels) || !positive(needle.lineWidthPixels)
        || !positive(needle.shadowWidthPixels) || !finite(needle.shadowOffset.x())
        || !finite(needle.shadowOffset.y()) || !positive(pivot.minimumRadiusPixels)
        || !positive(pivot.radiusWidthFactor) || !(pivot.glowRadiusFactor > 1.0)
        || !(pivot.glowMiddleFactor > 0.0 && pivot.glowMiddleFactor < 1.0)
        || pivot.glowCenterAlpha < 0 || pivot.glowCenterAlpha > 255
        || pivot.glowMiddleAlpha < 0 || pivot.glowMiddleAlpha > 255
        || !positive(pivot.rimWidthPixels)) {
        return fail(QStringLiteral("standard S-meter needle or pivot is invalid"));
    }
    if (!nonNegative(peakMarker.radiusInsetPixels) || !positive(peakMarker.lengthPixels)
        || !positive(peakMarker.halfWidthPixels) || !nonNegative(peakMarker.minimumLeadDb)
        || !finite(peakHold.innerRadiusOffsetPixels)
        || !finite(peakHold.outerRadiusOffsetPixels)
        || peakHold.innerRadiusOffsetPixels >= peakHold.outerRadiusOffsetPixels
        || !positive(peakHold.lineWidthPixels) || !nonNegative(peakHold.visibleAboveMinimumDb)) {
        return fail(QStringLiteral("standard S-meter peak geometry is invalid"));
    }
    if (readout.sourceFontMinimumPixels <= 0 || !positive(readout.sourceFontHeightDivisor)
        || readout.valueFontMinimumPixels <= 0 || !positive(readout.valueFontHeightDivisor)
        || readout.topExtraPixels < 0 || readout.sideMarginPixels < 0) {
        return fail(QStringLiteral("standard S-meter readout geometry is invalid"));
    }

    const QVector<QSizeF> validationSizes = {
        QSizeF(sizing.minimum),
        QSizeF(sizing.preferred),
        QSizeF(sizing.preferred.width() * 2.0, sizing.preferred.height()),
        QSizeF(sizing.preferred.width(), sizing.preferred.height() * 1.5),
        QSizeF(sizing.minimum.width(), sizing.minimum.height() * 20.0),
        QSizeF(sizing.minimum.width() * 10.0, sizing.minimum.height())};
    for (const QSizeF& size : validationSizes) {
        const Layout layout = layoutFor(size);
        const double markerRadius = layout.radius - peakMarker.radiusInsetPixels;
        const double peakHoldInnerRadius = layout.radius + peakHold.innerRadiusOffsetPixels;
        if (!positive(layout.radius) || !positive(layout.innerRadius)
            || !positive(markerRadius) || !positive(peakHoldInnerRadius)
            || !finite(layout.centerX) || !finite(layout.centerY)
            || !finite(layout.needlePivotY) || !positive(layout.pivotRadius)
            || !layout.viewport.isValid()
            || layout.viewport.left() < 0.0 || layout.viewport.top() < 0.0
            || layout.viewport.right() > size.width()
            || layout.viewport.bottom() > size.height()
            || layout.centerX <= layout.viewport.left()
            || layout.centerX >= layout.viewport.right()
            || layout.pivotRadius >= layout.viewport.width() * 0.5
            || layout.pivotRadius >= layout.viewport.height()) {
            return fail(QStringLiteral("standard S-meter derived layout is invalid"));
        }

        const QPointF center(layout.centerX, layout.centerY);
        const QPointF pivotPoint(layout.centerX, layout.needlePivotY);
        const double pivotDistance = std::hypot(
            pivotPoint.x() - center.x(), pivotPoint.y() - center.y());
        const double smallestMovementRadius =
            std::min({layout.innerRadius, markerRadius, peakHoldInnerRadius});
        if (!(pivotDistance < smallestMovementRadius)) {
            return fail(QStringLiteral("standard S-meter pivot must remain inside movement arcs"));
        }

        for (const double fraction : {0.0, 0.5, 1.0}) {
            const MovementRay ray = movementRayFor(size, fraction);
            const double directionLength = std::hypot(ray.direction.x(), ray.direction.y());
            const std::optional<QPointF> innerPoint =
                movementRayCircleIntersection(size, fraction, layout.innerRadius);
            if (!nearlyEqual(directionLength, 1.0, 1.0e-8) || !innerPoint
                || ray.scalePoint.x() < layout.viewport.left()
                || ray.scalePoint.x() > layout.viewport.right()
                || ray.scalePoint.y() < layout.viewport.top()
                || ray.scalePoint.y() > layout.viewport.bottom()) {
                return fail(QStringLiteral("standard S-meter movement ray is invalid"));
            }
        }
    }
    return true;
}

SMeterGeometry::Layout SMeterGeometry::layoutFor(const QSizeF& size) const
{
    Layout layout;
    const double width = std::max(size.width(), 1.0);
    const double height = std::max(size.height(), 1.0);
    const double aspectRatio = width / height;
    layout.viewport = QRectF(0.0, 0.0, width, height);
    if (aspectRatio < sizing.minimumAspectRatio) {
        const double viewportHeight = width / sizing.minimumAspectRatio;
        layout.viewport.setTop((height - viewportHeight) * 0.5);
        layout.viewport.setHeight(viewportHeight);
    } else if (aspectRatio > sizing.maximumAspectRatio) {
        const double viewportWidth = height * sizing.maximumAspectRatio;
        layout.viewport.setLeft((width - viewportWidth) * 0.5);
        layout.viewport.setWidth(viewportWidth);
    }

    const double faceWidth = layout.viewport.width();
    const double faceHeight = layout.viewport.height();
    layout.centerX = layout.viewport.left() + faceWidth * arc.centerXWidthFactor;
    layout.radius = faceWidth * arc.radiusWidthFactor;
    layout.centerY = layout.viewport.top()
        + layout.radius + faceHeight * arc.centerYHeightFactor;
    layout.needlePivotY = layout.viewport.bottom() + needle.pivotYBelowWidgetPixels;
    layout.innerRadius = layout.radius - arc.innerGapPixels;
    const double preferredAspect = static_cast<double>(sizing.preferred.width())
        / static_cast<double>(sizing.preferred.height());
    const double pivotScaleWidth = std::min(faceWidth, faceHeight * preferredAspect);
    layout.pivotRadius = std::max(
        pivot.minimumRadiusPixels, pivotScaleWidth * pivot.radiusWidthFactor);
    layout.tickFontPixels = std::max(
        tickStyle.fontMinimumPixels,
        static_cast<int>(std::floor(faceHeight * tickStyle.fontHeightFactor)));
    layout.sourceFontPixels =
        std::max(readout.sourceFontMinimumPixels,
                 static_cast<int>(std::floor(faceHeight / readout.sourceFontHeightDivisor)));
    layout.valueFontPixels =
        std::max(readout.valueFontMinimumPixels,
                 static_cast<int>(std::floor(faceHeight / readout.valueFontHeightDivisor)));
    return layout;
}

double SMeterGeometry::fractionToRadians(double fraction) const
{
    const double start = qDegreesToRadians(arc.startDegrees);
    const double end = qDegreesToRadians(arc.endDegrees);
    return end - std::clamp(fraction, 0.0, 1.0) * (end - start);
}

double SMeterGeometry::rxFraction(double dbm) const
{
    const double clamped = std::clamp(dbm, rxScale.minimumDbm, rxScale.maximumDbm);
    if (clamped <= rxScale.s9Dbm) {
        return rxScale.s9Fraction * (clamped - rxScale.minimumDbm)
            / (rxScale.s9Dbm - rxScale.minimumDbm);
    }
    return rxScale.s9Fraction
        + (1.0 - rxScale.s9Fraction) * (clamped - rxScale.s9Dbm)
            / (rxScale.maximumDbm - rxScale.s9Dbm);
}

double SMeterGeometry::scaleFraction(const StaticScale& scale, double value) const
{
    return std::clamp((value - scale.minimum) / (scale.maximum - scale.minimum), 0.0, 1.0);
}

SMeterGeometry::MovementRay SMeterGeometry::movementRayFor(
    const QSizeF& size, double fraction) const
{
    const Layout layout = layoutFor(size);
    const double angle = fractionToRadians(fraction);
    const QPointF pivot(layout.centerX, layout.needlePivotY);
    const QPointF scalePoint(layout.centerX + layout.radius * std::cos(angle),
                             layout.centerY - layout.radius * std::sin(angle));
    const QPointF delta = scalePoint - pivot;
    const double length = std::hypot(delta.x(), delta.y());
    const QPointF direction = length > 0.0 ? delta / length : QPointF();
    return {pivot, scalePoint, direction};
}

std::optional<QPointF> SMeterGeometry::movementRayCircleIntersection(
    const QSizeF& size, double fraction, double radius) const
{
    if (!positive(radius)) {
        return std::nullopt;
    }

    const Layout layout = layoutFor(size);
    const MovementRay ray = movementRayFor(size, fraction);
    const double directionLength = std::hypot(ray.direction.x(), ray.direction.y());
    if (directionLength <= 0.0) {
        return std::nullopt;
    }

    const QPointF center(layout.centerX, layout.centerY);
    const QPointF relativePivot = ray.pivot - center;
    const double projection = QPointF::dotProduct(relativePivot, ray.direction);
    const double constant = QPointF::dotProduct(relativePivot, relativePivot) - radius * radius;
    const double discriminant = projection * projection - constant;
    if (discriminant < 0.0) {
        return std::nullopt;
    }

    const double distance = -projection + std::sqrt(discriminant);
    if (distance < 0.0 || !finite(distance)) {
        return std::nullopt;
    }
    return ray.pivot + distance * ray.direction;
}

QPointF SMeterGeometry::needleTip(const QSizeF& size, double fraction) const
{
    const MovementRay ray = movementRayFor(size, fraction);
    return ray.scalePoint + needle.tipExtensionPixels * ray.direction;
}

const SMeterGeometry::PowerTickPolicy& SMeterGeometry::powerTickPolicy(double maximumWatts) const
{
    for (const PowerTickPolicy& policy : powerTickPolicies) {
        if (maximumWatts >= policy.minimumScaleWatts) {
            return policy;
        }
    }
    return powerTickPolicies.constLast();
}

} // namespace AetherSDR
