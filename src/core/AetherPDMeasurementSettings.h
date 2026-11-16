#pragma once

#include "RadioSettingsScope.h"
#include "txlinearity/SpectrumWindow.h"
#include <QString>
#include <utility>

namespace AetherSDR {
// Client measurement preferences; never source/drive state or permission to transmit.
class AetherPDMeasurementSettings final {
public:
    static constexpr int kSchemaVersion = 2;
    // Matches the bounded snapshot retrieval budget of the installed source.
    static constexpr int kMaxAverages = 4;
    static constexpr int kMinTimeoutMs = 3000;
    static constexpr int kMaxTimeoutMs = 10000;
    static QString featureName() { return QStringLiteral("AetherPDMeasurement"); }
    struct Values {
        txlin::WindowKind window{txlin::WindowKind::BlackmanHarris7};
        int averages{4};
        int maxOrder{7};
        int timeoutMs{kMaxTimeoutMs};
        bool operator==(const Values&) const = default;
    };
    struct ReadResult {
        bool usable{false};
        Values values;
        QString reason;
    };
    explicit AetherPDMeasurementSettings(RadioSettingsScope scope) : m_scope(std::move(scope)) {}
    ReadResult load() const;
    bool save(const Values& values, QString& reason) const;
    static bool valid(const Values& values);
    static bool validTimeout(int timeoutMs);
private:
    static bool decode(const QJsonObject& object, Values& values, bool legacy);
    RadioSettingsScope m_scope;
};
} // namespace AetherSDR
