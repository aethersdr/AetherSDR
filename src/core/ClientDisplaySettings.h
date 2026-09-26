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
    struct FftAverage {
        int average = 0;
        bool weighted = false;
        bool operator==(const FftAverage&) const = default;
    };

    static std::optional<FftAverage> fftAverage(const RadioSettingsScope& scope,
                                               int panIndex, bool clientOwns)
    {
        if (!clientOwns || scope.radioId().isEmpty() || panIndex < 0) { return std::nullopt; }
        int version = 0;
        const QJsonObject doc = scope.featureExact(QStringLiteral("ClientDisplay"), &version);
        if (version != 1) { return std::nullopt; }
        return decodeAverage(doc.value(QStringLiteral("fftAverages")).toObject()
                                  .value(QString::number(panIndex)));
    }

    static bool saveFftAverage(const RadioSettingsScope& scope, int panIndex,
                               bool clientOwns, const FftAverage& value)
    {
        if (!clientOwns || scope.radioId().isEmpty() || panIndex < 0
            || value.average < 0 || value.average > 100) { return false; }
        int version = 0;
        AppSettings::FeatureReadStatus status;
        QJsonObject doc = scope.featureExact(QStringLiteral("ClientDisplay"), &version, &status);
        const QJsonValue field = doc.value(QStringLiteral("fftAverages"));
        QJsonObject averages = field.toObject();
        const QString key = QString::number(panIndex);
        if (status == AppSettings::FeatureReadStatus::Corrupt
            || status == AppSettings::FeatureReadStatus::Unavailable
            || (status == AppSettings::FeatureReadStatus::Present && version != 1)
            || (!field.isUndefined() && !field.isObject())
            || (averages.contains(key) && !decodeAverage(averages.value(key)))) {
            qWarning() << "ClientDisplay: refusing to replace unreadable or newer averaging settings";
            return false;
        }
        QJsonObject row = averages.value(key).toObject();
        row.insert(QStringLiteral("average"), value.average);
        row.insert(QStringLiteral("weighted"), value.weighted);
        averages.insert(key, row);
        doc.insert(QStringLiteral("fftAverages"), averages);
        if (!scope.setFeature(QStringLiteral("ClientDisplay"), 1, doc)) {
            qWarning() << "ClientDisplay: averaging settings write did not persist";
            return false;
        }
        return true;
    }

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

private:
    static std::optional<FftAverage> decodeAverage(const QJsonValue& value)
    {
        if (!value.isObject()) { return std::nullopt; }
        const QJsonObject row = value.toObject();
        const QJsonValue average = row.value(QStringLiteral("average"));
        const QJsonValue weighted = row.value(QStringLiteral("weighted"));
        const int number = average.toInt(-1);
        if (!average.isDouble() || average.toDouble() != number || number < 0
            || number > 100 || !weighted.isBool()) { return std::nullopt; }
        return FftAverage{number, weighted.toBool()};
    }
};

} // namespace AetherSDR
