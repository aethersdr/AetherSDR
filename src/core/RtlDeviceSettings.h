#pragma once

#include "RadioSettingsScope.h"

namespace AetherSDR {

// Device calibration belongs to the reported serial, never to a USB index or
// the family's default row. This owner receives only adopted runtime values;
// OperatingState and the UI never write this feature document.
class RtlDeviceSettings final {
public:
    static constexpr int kSchemaVersion = 1;
    static constexpr int kMinPpm = -1000;
    static constexpr int kMaxPpm = 1000;
    static QString featureName() { return QStringLiteral("RtlDevice"); }
    struct Values {
        int ppm = 0;
        bool dcSuppression = false;
        bool operator==(const Values&) const = default;
    };
    enum class ReadStatus { Missing, Ready, Refused };
    struct ReadResult {
        ReadStatus status = ReadStatus::Missing;
        Values values;
        QString reason;
    };

    explicit RtlDeviceSettings(RadioSettingsScope scope) : m_scope(std::move(scope)) {}
    ReadResult load() const;
    bool saveAccepted(const Values& values, QString& reason) const;
    static bool decode(const QJsonObject& object, Values& values);

private:
    bool hasDeviceIdentity() const;
    RadioSettingsScope m_scope;
};

} // namespace AetherSDR
