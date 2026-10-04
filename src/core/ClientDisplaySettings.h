#pragma once

#include "DbmRangePlausibility.h"
#include "RadioSettingsScope.h"
#include "WaterfallRate.h"

#include <QDebug>
#include <QJsonValue>
#include <optional>

namespace AetherSDR {

// Client-shaped display state (waterfallRates, fftAverages, fftFps, dbmRanges),
// keyed by pan slot in the schema-1 `ClientDisplay` document. Only for a radio
// whose display this engine shapes (`shapedLocally`); a Flex stores and reports
// these itself (#2465, #4126). Radio publications and adaptive caps never write
// here. Tables are optional and every writer is read-modify-write: no bump.
class ClientDisplaySettings {
public:
    // The Display panel's own FFT FPS slider bounds (SpectrumOverlayMenu). A
    // stored value outside them was not written by that panel.
    static constexpr int kFftFpsMin = 5;
    static constexpr int kFftFpsMax = 60;

    struct DbmRange {
        float minDbm{0.0f};
        float maxDbm{0.0f};
    };

    // May the client store and restore this pan's dBm range? Only where the
    // engine shapes the display AND the bins are absolute levels computed here
    // (panBinsAbsolute): a Flex echoes its range, and an Icom's backend
    // publishes one from scope calibration that a stored copy would fight.
    static constexpr bool clientOwnsDbmRange(bool shapedLocally,
                                             bool panBinsAbsolute) noexcept
    {
        return shapedLocally && panBinsAbsolute;
    }

    // DeferredSettingsWrites key for one pending edit, per (radio, pan slot,
    // field): that queue keeps the last write per key, so the waterfall rate and
    // FFT FPS of one pan changed together must not share a key.
    static QString pendingWriteKey(const RadioSettingsScope& scope, int panIndex,
                                   const char* field)
    {
        return QString::number(scope.family().size()) + QLatin1Char(':') + scope.family()
            + QString::number(scope.radioId().size()) + QLatin1Char(':') + scope.radioId()
            + QLatin1Char(':') + QString::number(panIndex)
            + QLatin1Char(':') + QLatin1String(field);
    }

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

    static std::optional<int> fftFps(const RadioSettingsScope& scope,
                                     int panIndex, bool shapedLocally)
    {
        return boundedInt(readEntry(scope, panIndex, shapedLocally,
                                    QStringLiteral("fftFps")),
                          kFftFpsMin, kFftFpsMax);
    }

    static void saveFftFps(const RadioSettingsScope& scope, int panIndex,
                           bool shapedLocally, int fps)
    {
        if (fps < kFftFpsMin || fps > kFftFpsMax) {
            return;
        }
        writeEntry(scope, panIndex, shapedLocally, QStringLiteral("fftFps"), fps);
    }

    // `clientOwned` is clientOwnsDbmRange(), not shapesDisplayRatesLocally()
    // alone.
    static std::optional<DbmRange> dbmRange(const RadioSettingsScope& scope,
                                            int panIndex, bool clientOwned)
    {
        const QJsonValue value = readEntry(scope, panIndex, clientOwned,
                                           QStringLiteral("dbmRanges"));
        if (!value.isObject()) {
            return std::nullopt;
        }
        const QJsonObject range = value.toObject();
        const QJsonValue minValue = range.value(QStringLiteral("min"));
        const QJsonValue maxValue = range.value(QStringLiteral("max"));
        if (!minValue.isDouble() || !maxValue.isDouble()) {
            return std::nullopt;
        }
        const double minRaw = minValue.toDouble();
        const double maxRaw = maxValue.toDouble();
        if (!dbmNarrowsToFloat(minRaw) || !dbmNarrowsToFloat(maxRaw)) {
            return std::nullopt;
        }
        const DbmRange result{static_cast<float>(minRaw),
                              static_cast<float>(maxRaw)};
        if (!dbmRangeLooksPlausible(result.minDbm, result.maxDbm)) {
            return std::nullopt;
        }
        return result;
    }

    static void saveDbmRange(const RadioSettingsScope& scope, int panIndex,
                             bool clientOwned, float minDbm, float maxDbm)
    {
        if (!dbmRangeLooksPlausible(minDbm, maxDbm)) {
            return;
        }
        writeEntry(scope, panIndex, clientOwned, QStringLiteral("dbmRanges"),
                   QJsonObject{{QStringLiteral("min"), static_cast<double>(minDbm)},
                               {QStringLiteral("max"), static_cast<double>(maxDbm)}});
    }

private:
    // One pan slot's entry in one table, or an undefined value when this radio
    // does not keep it here, the identity is unknown, or the document is not a
    // schema this build reads.
    static QJsonValue readEntry(const RadioSettingsScope& scope, int panIndex,
                                bool clientOwned, const QString& table)
    {
        if (!clientOwned || !scope.hasRadioIdentity() || panIndex < 0) {
            return QJsonValue(QJsonValue::Undefined);
        }
        int version = 0;
        const QJsonObject doc = scope.featureExact(QStringLiteral("ClientDisplay"), &version);
        if (version != 1) {
            return QJsonValue(QJsonValue::Undefined);
        }
        return doc.value(table).toObject().value(QString::number(panIndex));
    }

    // Whole numbers only: 12.5 is not a slider position, and toInt() would
    // quietly make it one.
    static std::optional<int> boundedInt(const QJsonValue& value, int min, int max)
    {
        const int number = value.toInt(min - 1);
        if (!value.isDouble() || value.toDouble() != number
            || number < min || number > max) {
            return std::nullopt;
        }
        return number;
    }

    // Read-modify-write on the exact row, so the other tables survive and a
    // newer or unreadable document is left alone.
    static void writeEntry(const RadioSettingsScope& scope, int panIndex,
                           bool clientOwned, const QString& table,
                           const QJsonValue& value)
    {
        if (!clientOwned || !scope.hasRadioIdentity() || panIndex < 0) {
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
        QJsonObject entries = doc.value(table).toObject();
        entries.insert(QString::number(panIndex), value);
        doc.insert(table, entries);
        if (!scope.setFeature(QStringLiteral("ClientDisplay"), 1, doc)) {
            qWarning() << "ClientDisplay: settings write did not persist";
        }
    }

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
