#pragma once

#include "RadioSettingsScope.h"
#include "RadioSettingsIdentity.h"
#include "SharedCapturePolicy.h"

#include <QMap>
#include <QStringList>

namespace AetherSDR {

// RFC #5468 F3a: storage and planning foundation. Runtime ownership remains
// OperatingState until the accepted-capture adapter lands. Do not call migrate
// or patch from that old runtime: cutover must disable its overlapping domains
// and use RadioStateMemory::storeRtlRfGainPreservingLegacy for later gain saves.
class RtlSliceSettings {
public:
    static constexpr int kSchemaVersion = 1;
    static QString featureName() { return QStringLiteral("RtlSlices"); }
    struct Slice {
        int id = -1;
        double frequencyHz = 0;
        QString mode;
        double filterLowHz = 0;
        double filterHighHz = 0;
        // Existing SliceModel defaults, also used for fields legacy RTL never saved.
        QString agcMode = QStringLiteral("med");
        int agcThreshold = 65;
        bool squelchEnabled = false;
        int squelchLevel = 20;
        int audioGain = 50;
        bool audioMute = false;
        int audioPan = 50;
    };
    struct Document {
        double captureCenterHz = 0;
        double sampleRateHz = 0;
        QMap<int, Slice> slices;
    };
    enum class ReadStatus { Missing, Ready, Refused };
    struct ReadResult {
        ReadStatus status = ReadStatus::Missing;
        Document document;
        QString reason;
    };
    enum class MigrationResult { Claimed, Settled, NoSource, Retry };

    explicit RtlSliceSettings(RadioSettingsScope scope) : m_scope(std::move(scope)) {}
    // Scope comes from RadioModel::settingsScope(), not a backend USB re-read.
    ReadResult load() const;
    static bool decode(const QJsonObject& object, Document& output, QString& reason);

    // Only accepted runtime entries belong here. Omitted entries survive partial
    // restore/capacity reduction; explicit removals are separate stable IDs.
    bool patch(double captureCenterHz, double sampleRateHz,
               const QVector<Slice>& accepted, const QVector<int>& removed = {}) const;
    MigrationResult migrateLegacy(const RadioSerialIdentity& identity) const;

    // Adapter provides mode/BFO/guard metadata for EVERY saved entry. This
    // class never guesses usable margins, CW translation or receiver capacity.
    static SharedCapturePolicy::RestoreResult planRestore(
        const Document& saved, const SharedCapturePolicy::CaptureDescriptor& actual,
        std::span<const SharedCapturePolicy::SliceDescriptor> prepared,
        std::span<const SharedCapturePolicy::CenterDomain> domains,
        SharedCapturePolicy::ReceiverLimits limits);

private:
    RadioSettingsScope m_scope;
};

} // namespace AetherSDR
