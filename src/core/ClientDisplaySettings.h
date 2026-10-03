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
    // ---- FFT averaging, and the dBm range ----
    //
    // Both are display state THIS CLIENT owns on a radio that cannot save and
    // recall them (Principle III's deciding test). They live in the same
    // per-radio, per-pan document as the waterfall cadence above and for the
    // same reason: a level that suits one radio is wrong on the next, and the
    // global per-widget keys the rest of the display uses cannot tell them
    // apart.
    //
    // NOT written for a radio that reports its own value back. The caller
    // carries that predicate -- backendPanAveraging for the averaging,
    // radioOwnsDbmScale for the range -- because it is a capability question
    // and this class has no backend.

    // 0..kMaxFftAverage frames; nullopt when unset or unusable.
    static constexpr int kMaxFftAverage = 100;

    static std::optional<int> fftAverage(const RadioSettingsScope& scope, int panIndex)
    {
        return readInt(scope, QStringLiteral("fftAverages"), panIndex, 0, kMaxFftAverage);
    }

    static void saveFftAverage(const RadioSettingsScope& scope, int panIndex, int frames)
    {
        if (frames < 0 || frames > kMaxFftAverage) {
            return;
        }
        writeInt(scope, QStringLiteral("fftAverages"), panIndex, frames);
    }

    static std::optional<bool> fftWeightedAverage(const RadioSettingsScope& scope, int panIndex)
    {
        const std::optional<int> v =
            readInt(scope, QStringLiteral("fftWeightedAverages"), panIndex, 0, 1);
        if (!v) {
            return std::nullopt;
        }
        return *v != 0;
    }

    static void saveFftWeightedAverage(const RadioSettingsScope& scope, int panIndex, bool on)
    {
        writeInt(scope, QStringLiteral("fftWeightedAverages"), panIndex, on ? 1 : 0);
    }

    struct DbmRange {
        float minDbm = 0.0f;
        float maxDbm = 0.0f;
    };

    // Bounds wide enough for any receiver's scale and narrow enough that a
    // corrupt document cannot park the display somewhere unrecoverable.
    static constexpr double kMinDbm = -250.0;
    static constexpr double kMaxDbm = 100.0;

    static std::optional<DbmRange> dbmRange(const RadioSettingsScope& scope, int panIndex)
    {
        if (!scope.hasRadioIdentity() || panIndex < 0) {
            return std::nullopt;
        }
        int version = 0;
        const QJsonObject doc = scope.featureExact(QStringLiteral("ClientDisplay"), &version);
        if (version != 1) {
            return std::nullopt;
        }
        const QJsonObject entry = doc.value(QStringLiteral("dbmRanges")).toObject()
                                     .value(QString::number(panIndex)).toObject();
        const QJsonValue lo = entry.value(QStringLiteral("min"));
        const QJsonValue hi = entry.value(QStringLiteral("max"));
        if (!lo.isDouble() || !hi.isDouble()) {
            return std::nullopt;
        }
        // ORDERED AND IN RANGE, or nothing. Half a range is not a range, and an
        // inverted one renders as an empty scale with no way back from the UI.
        if (!(lo.toDouble() < hi.toDouble())
            || lo.toDouble() < kMinDbm || hi.toDouble() > kMaxDbm) {
            return std::nullopt;
        }
        return DbmRange{static_cast<float>(lo.toDouble()),
                        static_cast<float>(hi.toDouble())};
    }

    static void saveDbmRange(const RadioSettingsScope& scope, int panIndex,
                             float minDbm, float maxDbm)
    {
        if (!scope.hasRadioIdentity() || panIndex < 0 || !(minDbm < maxDbm)
            || minDbm < kMinDbm || maxDbm > kMaxDbm) {
            return;
        }
        QJsonObject doc;
        if (!loadForWrite(scope, &doc)) {
            return;
        }
        QJsonObject ranges = doc.value(QStringLiteral("dbmRanges")).toObject();
        ranges.insert(QString::number(panIndex),
                      QJsonObject{{QStringLiteral("min"), minDbm},
                                  {QStringLiteral("max"), maxDbm}});
        doc.insert(QStringLiteral("dbmRanges"), ranges);
        store(scope, doc);
    }

private:
    // The read and write halves the three accessors above share. Kept private:
    // the section name is part of this class's schema, not its interface.
    static std::optional<int> readInt(const RadioSettingsScope& scope,
                                      const QString& section, int panIndex,
                                      int lo, int hi)
    {
        if (!scope.hasRadioIdentity() || panIndex < 0) {
            return std::nullopt;
        }
        int version = 0;
        const QJsonObject doc = scope.featureExact(QStringLiteral("ClientDisplay"), &version);
        if (version != 1) {
            return std::nullopt;
        }
        const QJsonValue value = doc.value(section).toObject()
                                    .value(QString::number(panIndex));
        const int v = value.toInt(lo - 1);
        // isDouble() AND an exact round-trip: a fractional or string value is a
        // corrupt document, not a number to truncate.
        if (!value.isDouble() || value.toDouble() != v || v < lo || v > hi) {
            return std::nullopt;
        }
        return v;
    }

    static void writeInt(const RadioSettingsScope& scope, const QString& section,
                         int panIndex, int value)
    {
        if (!scope.hasRadioIdentity() || panIndex < 0) {
            return;
        }
        QJsonObject doc;
        if (!loadForWrite(scope, &doc)) {
            return;
        }
        QJsonObject section_ = doc.value(section).toObject();
        section_.insert(QString::number(panIndex), value);
        doc.insert(section, section_);
        store(scope, doc);
    }

    // Same refusal as saveWaterfallRate(): never replace a document this build
    // cannot read, or one a newer build wrote.
    static bool loadForWrite(const RadioSettingsScope& scope, QJsonObject* out)
    {
        int version = 0;
        AppSettings::FeatureReadStatus status;
        QJsonObject doc = scope.featureExact(QStringLiteral("ClientDisplay"), &version, &status);
        if (version > 1 || status == AppSettings::FeatureReadStatus::Corrupt
            || status == AppSettings::FeatureReadStatus::Unavailable) {
            qWarning() << "ClientDisplay: refusing to replace unreadable or newer settings";
            return false;
        }
        *out = doc;
        return true;
    }

    static void store(const RadioSettingsScope& scope, const QJsonObject& doc)
    {
        if (!scope.setFeature(QStringLiteral("ClientDisplay"), 1, doc)) {
            qWarning() << "ClientDisplay: settings write did not persist";
        }
    }
};

} // namespace AetherSDR
