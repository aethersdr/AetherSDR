#include "AetherPDMeasurementSettings.h"

#include <QJsonValue>
#include <QLoggingCategory>
#include <cmath>
#include <optional>

namespace AetherSDR {
namespace {
Q_LOGGING_CATEGORY(lcPDMeasurementSettings, "aether.waveform.measurement.settings")
using ReadStatus = AppSettings::FeatureReadStatus;
}
bool AetherPDMeasurementSettings::valid(const Values& values)
{
    return values.averages >= 1 && values.averages <= kMaxAverages
        && (values.maxOrder == 3 || values.maxOrder == 5 || values.maxOrder == 7)
        && (values.window == txlin::WindowKind::BlackmanHarris7 || values.window == txlin::WindowKind::BlackmanHarris4
            || values.window == txlin::WindowKind::FlatTop || values.window == txlin::WindowKind::Hann);
}
bool AetherPDMeasurementSettings::decode(const QJsonObject& object, Values& values)
{
    const QJsonValue averages = object.value(QStringLiteral("averages"));
    const QJsonValue order = object.value(QStringLiteral("maxOrder"));
    const QJsonValue window = object.value(QStringLiteral("window"));
    if (!averages.isDouble() || !order.isDouble() || !window.isString()
        || averages.toDouble() != std::floor(averages.toDouble())
        || order.toDouble() != std::floor(order.toDouble())) { return false; }
    Values candidate;
    candidate.averages = averages.toInt(0);
    candidate.maxOrder = order.toInt(0);
    candidate.window = txlin::windowKindFromKey(window.toString().toStdString());
    if (!valid(candidate) || QString::fromStdString(txlin::windowKindKey(candidate.window)) != window.toString()) { return false; }
    values = candidate;
    return true;
}
AetherPDMeasurementSettings::ReadResult AetherPDMeasurementSettings::load() const
{
    ReadResult result;
    if (!m_scope.isValid() || m_scope.radioId().isEmpty()) {
        result.reason = QStringLiteral("A reported radio identity is required for analysis preferences.");
        return result;
    }
    int schema = 0;
    ReadStatus status;
    const QJsonObject object = m_scope.featureExact(featureName(), &schema, &status);
    result.usable = status == ReadStatus::Missing
        || (status == ReadStatus::Present && schema == kSchemaVersion && decode(object, result.values));
    if (!result.usable) {
        result.reason = QStringLiteral("Analysis preferences are unreadable or use an unsupported schema; stored data was preserved.");
    }
    return result;
}
bool AetherPDMeasurementSettings::save(const Values& values, QString& reason) const
{
    if (!valid(values)) { reason = QStringLiteral("Unsupported analysis window, averaging count or IMD order."); return false; }
    const ReadResult prior = load();
    if (!prior.usable) { reason = prior.reason; return false; }
    int schema = 0;
    ReadStatus status;
    QJsonObject object = m_scope.featureExact(featureName(), &schema, &status);
    Values parsed;
    if (status != ReadStatus::Missing && (status != ReadStatus::Present
        || schema != kSchemaVersion || !decode(object, parsed))) {
        reason = QStringLiteral("Analysis preferences changed or cannot be replaced.");
        return false;
    }
    const std::optional<QJsonObject> expected = status == ReadStatus::Present ? std::optional(object) : std::nullopt;
    object.insert(QStringLiteral("window"), QString::fromStdString(txlin::windowKindKey(values.window)));
    object.insert(QStringLiteral("averages"), values.averages);
    object.insert(QStringLiteral("maxOrder"), values.maxOrder);
    if (!m_scope.compareAndSetFeature(featureName(), schema, expected, kSchemaVersion, object)) {
        reason = QStringLiteral("Analysis preferences were not saved; the store is unavailable or changed.");
        qCWarning(lcPDMeasurementSettings) << reason;
        return false;
    }
    reason.clear();
    return true;
}
} // namespace AetherSDR
