#pragma once

#include "RadioSettingsScope.h"
#include "WaterfallRate.h"

#include <QDebug>
#include <optional>

namespace AetherSDR {

// Client-shaped waterfall cadence, scoped to radio and pan slot. Radio-owned
// display publications and transient adaptive caps must never write this store.
class ClientDisplaySettings {
public:
    static std::optional<int> waterfallRate(const RadioSettingsScope& scope,
                                             int panIndex, bool shapedLocally)
    {
        if (!shapedLocally || !scope.hasRadioIdentity() || panIndex < 0) {
            return std::nullopt;
        }
        int version = 0;
        const QJsonObject doc = scope.featureExact(QStringLiteral("ClientDisplay"), &version);
        if (version != 1) {
            return std::nullopt;
        }
        const QJsonValue value = doc.value(QStringLiteral("waterfallRates")).toObject()
                                    .value(QString::number(panIndex));
        const int rate = value.toInt(-1);
        if (!value.isDouble() || value.toDouble() != rate
            || rate < WaterfallRate::kMin || rate > WaterfallRate::kMax) {
            return std::nullopt;
        }
        return rate;
    }

    static void saveWaterfallRate(const RadioSettingsScope& scope, int panIndex,
                                  bool shapedLocally, int rate)
    {
        if (!shapedLocally || !scope.hasRadioIdentity() || panIndex < 0
            || rate < WaterfallRate::kMin || rate > WaterfallRate::kMax) {
            return;
        }
        int version = 0;
        AppSettings::FeatureReadStatus status;
        QJsonObject doc = scope.featureExact(QStringLiteral("ClientDisplay"), &version, &status);
        if (version > 1 || status == AppSettings::FeatureReadStatus::Corrupt
            || status == AppSettings::FeatureReadStatus::Unavailable) {
            qWarning() << "ClientDisplay: refusing to replace unreadable or newer settings";
            return;
        }
        QJsonObject rates = doc.value(QStringLiteral("waterfallRates")).toObject();
        rates.insert(QString::number(panIndex), rate);
        doc.insert(QStringLiteral("waterfallRates"), rates);
        if (!scope.setFeature(QStringLiteral("ClientDisplay"), 1, doc)) {
            qWarning() << "ClientDisplay: settings write did not persist";
        }
    }
};

} // namespace AetherSDR
