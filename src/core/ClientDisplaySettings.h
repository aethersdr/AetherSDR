#pragma once

#include "DbmRangePlausibility.h"
#include "RadioSettingsScope.h"
#include "WaterfallRate.h"

#include <QDebug>
#include <QJsonValue>
#include <optional>

namespace AetherSDR {

// Client-shaped display state, scoped to radio and pan slot, in the schema-1
// `ClientDisplay` feature document. Radio-owned display publications and
// transient adaptive caps must never write this store.
//
// What lives here, and for which radio:
//
//   waterfallRates   the waterfall rate, 1..100
//   fftFps           FFT FPS, the Display panel's 5..60
//   dbmRanges        the pan's dBm scale, {"min": .., "max": ..}
//
// Each is a table keyed by pan slot. All three are values a Flex stores itself
// and reports back, which is why the client deliberately keeps no copy of them
// there (#2465, #4126, #4261). A radio whose display the ENGINE shapes has no
// radio-side display state at all, so nothing gave them back after a restart:
// FFT FPS returned to the widget default and the dBm scale to the pan model's
// -130..-40.
//
// `shapedLocally` is RadioModel::shapesDisplayRatesLocally(). False means the
// radio owns the value: nothing is read and nothing is written. The dBm range
// needs one more term, clientOwnsDbmRange() below.
//
// The two tables after waterfallRates were added without a schema bump, on
// purpose. They are optional and independent: a build that predates them reads
// its own table and, because every writer here is read-modify-write on the
// whole document, keeps theirs intact. A bump would have made that older build
// refuse the waterfall rate it can read perfectly well.
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

    // May the client store and restore this pan's dBm range?
    //
    // Only where the scale is nothing but a view on this host: the engine
    // shapes the display AND the bins are absolute levels computed here
    // (RadioCapabilities::panBinsAbsolute()). A Flex adopts a range and echoes
    // it (not shaped locally). An Icom shapes locally but its backend publishes
    // the range from the scope calibration (bins not absolute), and a stored
    // copy would fight that the way a stored copy fought a Flex.
    static constexpr bool clientOwnsDbmRange(bool shapedLocally,
                                             bool panBinsAbsolute) noexcept
    {
        return shapedLocally && panBinsAbsolute;
    }

    // The key a caller hands DeferredSettingsWrites for one pending edit: one
    // per (radio, pan slot, FIELD). The scope is in it so a radio switch cannot
    // overwrite a pending edit. The field is in it because that queue keeps the
    // LAST write per key: the waterfall rate and the FFT FPS of one pan, both
    // changed inside its 250 ms, would otherwise replace each other and only
    // one would reach the store.
    static QString pendingWriteKey(const RadioSettingsScope& scope, int panIndex,
                                   const char* field)
    {
        return QString::number(scope.family().size()) + QLatin1Char(':') + scope.family()
            + QString::number(scope.radioId().size()) + QLatin1Char(':') + scope.radioId()
            + QLatin1Char(':') + QString::number(panIndex)
            + QLatin1Char(':') + QLatin1String(field);
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

    // ── FFT FPS ───────────────────────────────────────────────────────────
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

    // ── The dBm scale ─────────────────────────────────────────────────────
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
        const DbmRange result{static_cast<float>(minValue.toDouble()),
                              static_cast<float>(maxValue.toDouble())};
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
};

} // namespace AetherSDR
