#pragma once

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

struct SplitQsySettings {
    static constexpr int kMinimumThresholdHz = 1;
    static constexpr int kMaximumThresholdHz = 200000;
    static constexpr int kDefaultThresholdHz = 20;
    static constexpr int kVersion = 1;
    static constexpr const char* kSettingsKey = "SplitBehavior";

    bool closeSplitOnQsy{true};
    int thresholdHz{kDefaultThresholdHz};

    static SplitQsySettings fromJson(const QJsonObject& object)
    {
        SplitQsySettings settings;
        if (object.isEmpty()) {
            return settings;
        }

        const auto version = object.value(QStringLiteral("v"));
        if (!version.isDouble()
            || version.toDouble() != static_cast<double>(kVersion)) {
            return settings;
        }

        const auto closeOnQsy = object.value(QStringLiteral("closeSplitOnQsy"));
        if (closeOnQsy.isBool()) {
            settings.closeSplitOnQsy = closeOnQsy.toBool();
        }

        const auto threshold = object.value(QStringLiteral("thresholdHz"));
        if (threshold.isDouble()) {
            settings.thresholdHz = static_cast<int>(std::clamp(
                threshold.toDouble(),
                static_cast<double>(kMinimumThresholdHz),
                static_cast<double>(kMaximumThresholdHz)));
        }
        return settings;
    }

    QJsonObject toJson() const
    {
        QJsonObject object;
        object.insert(QStringLiteral("v"), kVersion);
        object.insert(QStringLiteral("closeSplitOnQsy"), closeSplitOnQsy);
        object.insert(QStringLiteral("thresholdHz"),
                      std::clamp(thresholdHz, kMinimumThresholdHz,
                                 kMaximumThresholdHz));
        return object;
    }

    static SplitQsySettings load()
    {
        auto& store = AppSettings::instance();
        if (!store.contains(QLatin1String(kSettingsKey))) {
            SplitQsySettings defaults;
            defaults.save();
            return defaults;
        }

        const QString json = store.value(QLatin1String(kSettingsKey)).toString();
        if (json.isEmpty()) {
            return {};
        }
        return fromJson(QJsonDocument::fromJson(json.toUtf8()).object());
    }

    void save() const
    {
        auto& settings = AppSettings::instance();
        settings.setValue(
            QLatin1String(kSettingsKey),
            QString::fromUtf8(QJsonDocument(toJson()).toJson(
                QJsonDocument::Compact)));
        settings.save();
    }
};

inline bool shouldCloseSplitOnQsy(const SplitQsySettings& settings,
                                  bool splitActive, bool rxSlice,
                                  bool qsyCloseSuppressed,
                                  double frequencyMhz,
                                  double referenceFrequencyMhz)
{
    const double thresholdMhz =
        static_cast<double>(settings.thresholdHz) / 1000000.0;
    return settings.closeSplitOnQsy && splitActive && rxSlice
        && !qsyCloseSuppressed
        && std::abs(frequencyMhz - referenceFrequencyMhz) > thresholdMhz;
}

} // namespace AetherSDR
