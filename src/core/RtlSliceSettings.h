#pragma once

#include "RadioSettingsScope.h"
#include "RadioSettingsIdentity.h"
#include "SharedCapturePolicy.h"
#include "core/backends/NoiseBlankerKind.h"
#include "core/backends/rtl/RtlTrackingNotches.h"

#include <optional>

#include <QMap>
#include <QStringList>

namespace AetherSDR {

// RFC #5468 storage and planning. The RTL accepted-capture adapter claims this
// owner only after readback, disables overlapping OperatingState domains, and
// uses storeRtlRfGainPreservingLegacy for later gain saves. Pending intent is
// never an input to patch().
class RtlSliceSettings {
public:
    static constexpr int kSchemaVersion = 1;
    static QString featureName() { return QStringLiteral("RtlSlices"); }
    struct Filter {
        double lowHz = 0;
        double highHz = 0;
        bool operator==(const Filter&) const = default;
    };
    struct Slice {
        int id = -1;
        double frequencyHz = 0;
        QString mode;
        double filterLowHz = 0;
        double filterHighHz = 0;
        QMap<QString, Filter> modeFilters;
        // Existing SliceModel defaults, also used for fields legacy RTL never saved.
        QString agcMode = QStringLiteral("med");
        int agcThreshold = 65;
        bool squelchEnabled = false;
        int squelchLevel = 20;
        int audioGain = 50;
        bool audioMute = false;
        int audioPan = 50;
        // Optional in schema 1; old documents retain the legacy 75 us default.
        int wfmDeemphasisUs = 75;
        bool wfmForceMono = false;
        bool automaticSquelch = false;
        int automaticSquelchMarginDb = 10;
        int agcOffLevel = 10;
        int cwPitchHz = 600;
        bool ritEnabled = false;
        int ritOffsetHz = 0;
        NoiseBlankerKind nbKind = NoiseBlankerKind::Off;
        int nbLevel = 50;
        NoiseBlankerFill nbFill = NoiseBlankerFill::Zero;
        bool apfEnabled = false;
        int apfLevel = 50;
        bool nrEnabled = false;
        int nrLevel = 50;
        bool anfEnabled = false;
        int anfLevel = 50;
        bool mnEnabled = false;
        int mnLevel = 50;

    };
    struct Document {
        double captureCenterHz = 0;
        double sampleRateHz = 0;
        QMap<int, Slice> slices;
        rtl::RtlTrackingNotches trackingNotches;
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
               const QVector<Slice>& accepted, const QVector<int>& removed = {},
               const std::optional<rtl::RtlTrackingNotches>& trackingNotches = {}) const;
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
