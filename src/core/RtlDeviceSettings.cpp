#include "RtlDeviceSettings.h"

#include <QLoggingCategory>
#include <cmath>

namespace AetherSDR {
namespace {
Q_LOGGING_CATEGORY(lcRtlDeviceSettings, "aethersdr.rtl.settings")
using Status = AppSettings::FeatureReadStatus;
}

bool RtlDeviceSettings::hasDeviceIdentity() const
{
    return m_scope.family() == QLatin1String("rtl") && !m_scope.radioId().isEmpty();
}

bool RtlDeviceSettings::decode(const QJsonObject& object, Values& values)
{
    Values parsed;
    if (object.isEmpty()) {
        values = parsed;
        return true;
    }
    const QJsonValue ppm = object.value(QStringLiteral("ppm"));
    const QJsonValue dc = object.value(QStringLiteral("dcSuppression"));
    const double number = ppm.toDouble();
    if (!ppm.isDouble() || !std::isfinite(number) || std::trunc(number) != number
        || number < kMinPpm || number > kMaxPpm || !dc.isBool()) {
        return false;
    }
    parsed.ppm = static_cast<int>(number);
    parsed.dcSuppression = dc.toBool();
    values = parsed;
    return true;
}

RtlDeviceSettings::ReadResult RtlDeviceSettings::load() const
{
    ReadResult result;
    if (!hasDeviceIdentity()) {
        result.status = ReadStatus::Refused;
        result.reason = QStringLiteral("Session only: no reported device serial.");
        return result;
    }
    int version = 0;
    Status status;
    const QJsonObject object = m_scope.featureExact(featureName(), &version, &status);
    if (status == Status::Missing) {
        return result;
    }
    if (status == Status::Present && version == kSchemaVersion && decode(object, result.values)) {
        result.status = ReadStatus::Ready;
    } else {
        result.status = ReadStatus::Refused;
        result.reason = QStringLiteral("Stored device settings are unreadable or use an unsupported schema; preserved unchanged.");
    }
    return result;
}

bool RtlDeviceSettings::saveAccepted(const Values& values, QString& reason) const
{
    if (!hasDeviceIdentity()) {
        reason = QStringLiteral("Session only: no reported device serial.");
        return false;
    }
    int version = 0;
    Status status;
    QJsonObject object = m_scope.featureExact(featureName(), &version, &status);
    Values prior;
    if (status == Status::Unavailable || status == Status::Corrupt
        || (status == Status::Present && version != kSchemaVersion)
        || !decode(object, prior)) {
        reason = QStringLiteral("Applied for this session; stored device settings cannot be replaced.");
        qCWarning(lcRtlDeviceSettings) << reason;
        return false;
    }
    if (values.ppm < kMinPpm || values.ppm > kMaxPpm) {
        reason = QStringLiteral("Invalid accepted PPM correction.");
        qCWarning(lcRtlDeviceSettings) << reason;
        return false;
    }
    if (status == Status::Present && !object.isEmpty() && prior == values) {
        reason.clear();
        return true;
    }
    object.insert(QStringLiteral("ppm"), values.ppm);
    object.insert(QStringLiteral("dcSuppression"), values.dcSuppression);
    if (!m_scope.setFeature(featureName(), kSchemaVersion, object)) {
        reason = QStringLiteral("Applied for this session; saving device settings failed.");
        qCWarning(lcRtlDeviceSettings) << reason;
        return false;
    }
    reason.clear();
    return true;
}

} // namespace AetherSDR
