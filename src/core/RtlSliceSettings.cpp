#include "RtlSliceSettings.h"

#include "RadioStateMemory.h"

#include <QDebug>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace {
using Status = AppSettings::FeatureReadStatus;

bool number(const QJsonObject& object, const QString& key, double low, double high,
            double& value, bool integer = false)
{
    const QJsonValue field = object.value(key);
    value = field.toDouble();
    return field.isDouble() && std::isfinite(value) && value >= low && value <= high
        && (!integer || std::trunc(value) == value);
}

bool percent(const QJsonObject& object, const QString& key, int& value)
{
    double parsed = 0;
    if (!number(object, key, 0, 100, parsed, true)) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool stableId(const QString& text, int& id)
{
    bool ok = false;
    id = text.toInt(&ok);
    return ok && id >= 0 && id < static_cast<int>(SharedCapturePolicy::kMaxEntries)
        && QString::number(id) == text;
}

QJsonObject encodeSlice(const RtlSliceSettings::Slice& slice, QJsonObject previous = {})
{
    previous.insert(QStringLiteral("freq"), slice.frequencyHz);
    previous.insert(QStringLiteral("mode"), slice.mode);
    previous.insert(QStringLiteral("filterLow"), slice.filterLowHz);
    previous.insert(QStringLiteral("filterHigh"), slice.filterHighHz);
    QJsonObject filters = previous.value(QStringLiteral("modeFilters")).toObject();
    for (auto it = slice.modeFilters.cbegin(); it != slice.modeFilters.cend(); ++it) {
        QJsonObject filter = filters.value(it.key()).toObject();
        filter.insert(QStringLiteral("low"), it->lowHz);
        filter.insert(QStringLiteral("high"), it->highHz);
        filters.insert(it.key(), filter);
    }
    previous.insert(QStringLiteral("modeFilters"), filters);
    previous.insert(QStringLiteral("agcMode"), slice.agcMode);
    previous.insert(QStringLiteral("agcThreshold"), slice.agcThreshold);
    QJsonObject squelch = previous.value(QStringLiteral("squelch")).toObject();
    squelch.insert(QStringLiteral("enabled"), slice.squelchEnabled);
    squelch.insert(QStringLiteral("level"), slice.squelchLevel);
    squelch.insert(QStringLiteral("automatic"), slice.automaticSquelch);
    squelch.insert(QStringLiteral("marginDb"), slice.automaticSquelchMarginDb);
    previous.insert(QStringLiteral("squelch"), squelch);
    previous.insert(QStringLiteral("audioGain"), slice.audioGain);
    previous.insert(QStringLiteral("audioMute"), slice.audioMute);
    previous.insert(QStringLiteral("audioPan"), slice.audioPan);
    previous.insert(QStringLiteral("wfmDeemphasisUs"), slice.wfmDeemphasisUs);
    previous.insert(QStringLiteral("wfmForceMono"), slice.wfmForceMono);
    QJsonObject receive = previous.value(QStringLiteral("receiveDsp")).toObject();
    receive.insert(QStringLiteral("agcOffLevel"), slice.agcOffLevel);
    receive.insert(QStringLiteral("cwPitchHz"), slice.cwPitchHz);
    receive.insert(QStringLiteral("ritEnabled"), slice.ritEnabled);
    receive.insert(QStringLiteral("ritOffsetHz"), slice.ritOffsetHz);
    receive.insert(QStringLiteral("nbKind"), static_cast<int>(slice.nbKind));
    receive.insert(QStringLiteral("nbLevel"), slice.nbLevel);
    receive.insert(QStringLiteral("nbFill"), static_cast<int>(slice.nbFill));
    receive.insert(QStringLiteral("apfEnabled"), slice.apfEnabled);
    receive.insert(QStringLiteral("apfLevel"), slice.apfLevel);
    receive.insert(QStringLiteral("nrEnabled"), slice.nrEnabled);
    receive.insert(QStringLiteral("nrLevel"), slice.nrLevel);
    receive.insert(QStringLiteral("anfEnabled"), slice.anfEnabled);
    receive.insert(QStringLiteral("anfLevel"), slice.anfLevel);
    receive.insert(QStringLiteral("mnEnabled"), slice.mnEnabled);
    receive.insert(QStringLiteral("mnLevel"), slice.mnLevel);
    previous.insert(QStringLiteral("receiveDsp"), receive);
    return previous;
}

bool refuse(const QString& reason)
{
    qWarning() << "RtlSliceSettings:" << reason;
    return false;
}
} // namespace

bool RtlSliceSettings::decode(const QJsonObject& object, Document& output, QString& reason)
{
    Document parsed;
    if (object.isEmpty()) {
        output = parsed;
        reason.clear();
        return true; // A settled, deliberately empty document.
    }
    reason = QStringLiteral("malformed capture context or slice document");
    if (!number(object, QStringLiteral("captureCenterHz"), 0,
                SharedCapturePolicy::kMaxMagnitudeHz, parsed.captureCenterHz)
        || !number(object, QStringLiteral("sampleRateHz"), 0,
                   SharedCapturePolicy::kMaxMagnitudeHz, parsed.sampleRateHz)
        || parsed.captureCenterHz <= 0 || parsed.sampleRateHz <= 0
        || !object.value(QStringLiteral("slices")).isObject()) {
        return false;
    }
    const QJsonObject slices = object.value(QStringLiteral("slices")).toObject();
    if (slices.size() > static_cast<int>(SharedCapturePolicy::kMaxEntries)) {
        return false;
    }
    const QStringList modes{QStringLiteral("AM"), QStringLiteral("SAM"), QStringLiteral("FM"),
        QStringLiteral("FMN"), QStringLiteral("WFM"), QStringLiteral("USB"), QStringLiteral("LSB"),
        QStringLiteral("CW"), QStringLiteral("CWR")};
    const QStringList agcModes{QStringLiteral("off"), QStringLiteral("slow"),
        QStringLiteral("med"), QStringLiteral("fast")};
    for (auto it = slices.begin(); it != slices.end(); ++it) {
        Slice slice;
        reason = QStringLiteral("malformed slice %1").arg(it.key());
        if (!stableId(it.key(), slice.id) || !it.value().isObject()) {
            return false;
        }
        const QJsonObject entry = it.value().toObject();
        slice.mode = entry.value(QStringLiteral("mode")).toString();
        slice.agcMode = entry.value(QStringLiteral("agcMode")).toString();
        const double maximum = SharedCapturePolicy::kMaxMagnitudeHz;
        if (!number(entry, QStringLiteral("freq"), 0, maximum, slice.frequencyHz)
            || slice.frequencyHz <= 0
            || !number(entry, QStringLiteral("filterLow"), -maximum, maximum, slice.filterLowHz)
            || !number(entry, QStringLiteral("filterHigh"), -maximum, maximum, slice.filterHighHz)
            || slice.filterLowHz >= slice.filterHighHz || !modes.contains(slice.mode)
            || !agcModes.contains(slice.agcMode)
            || !percent(entry, QStringLiteral("agcThreshold"), slice.agcThreshold)
            || !percent(entry, QStringLiteral("audioGain"), slice.audioGain)
            || !percent(entry, QStringLiteral("audioPan"), slice.audioPan)
            || !entry.value(QStringLiteral("audioMute")).isBool()
            || !entry.value(QStringLiteral("squelch")).isObject()) {
            return false;
        }
        if (entry.contains(QStringLiteral("modeFilters"))) {
            if (!entry.value(QStringLiteral("modeFilters")).isObject()) { return false; }
            const QJsonObject filters = entry.value(QStringLiteral("modeFilters")).toObject();
            if (filters.size() > modes.size()) { return false; }
            for (auto filter = filters.begin(); filter != filters.end(); ++filter) {
                Filter value;
                if (!modes.contains(filter.key()) || !filter.value().isObject()
                    || !number(filter.value().toObject(), QStringLiteral("low"), -100000, 100000, value.lowHz, true)
                    || !number(filter.value().toObject(), QStringLiteral("high"), -100000, 100000, value.highHz, true)
                    || value.lowHz >= value.highHz) { return false; }
                slice.modeFilters.insert(filter.key(), value);
            }
        }
        // Optional schema-1 object: old documents use the existing UI defaults.
        if (entry.contains(QStringLiteral("receiveDsp"))) {
            if (!entry.value(QStringLiteral("receiveDsp")).isObject()) { return false; }
            const QJsonObject receive = entry.value(QStringLiteral("receiveDsp")).toObject();
            double pitch = 0, rit = 0, kind = 0, fill = 0;
            if (!percent(receive, QStringLiteral("agcOffLevel"), slice.agcOffLevel)
                || !number(receive, QStringLiteral("cwPitchHz"), 100, 6000, pitch, true)
                || !number(receive, QStringLiteral("ritOffsetHz"), -99999, 99999, rit, true)
                || !number(receive, QStringLiteral("nbKind"), 0, 2, kind, true)
                || !number(receive, QStringLiteral("nbFill"), 0, 4, fill, true)
                || !percent(receive, QStringLiteral("nbLevel"), slice.nbLevel)
                || !percent(receive, QStringLiteral("apfLevel"), slice.apfLevel)
                || !receive.value(QStringLiteral("ritEnabled")).isBool()
                || !receive.value(QStringLiteral("apfEnabled")).isBool()) { return false; }
            slice.cwPitchHz = static_cast<int>(pitch);
            slice.ritOffsetHz = static_cast<int>(rit);
            slice.nbKind = static_cast<NoiseBlankerKind>(static_cast<int>(kind));
            slice.nbFill = static_cast<NoiseBlankerFill>(static_cast<int>(fill));
            slice.ritEnabled = receive.value(QStringLiteral("ritEnabled")).toBool();
            slice.apfEnabled = receive.value(QStringLiteral("apfEnabled")).toBool();
            const auto optionalToggle = [&](const QString& key, bool& value) {
                if (!receive.contains(key)) { return true; }
                if (!receive.value(key).isBool()) { return false; }
                value = receive.value(key).toBool(); return true;
            };
            const auto optionalLevel = [&](const QString& key, int& value) {
                return !receive.contains(key) || percent(receive, key, value);
            };
            if (!optionalToggle(QStringLiteral("nrEnabled"), slice.nrEnabled)
                || !optionalToggle(QStringLiteral("anfEnabled"), slice.anfEnabled)
                || !optionalToggle(QStringLiteral("mnEnabled"), slice.mnEnabled)
                || !optionalLevel(QStringLiteral("nrLevel"), slice.nrLevel)
                || !optionalLevel(QStringLiteral("anfLevel"), slice.anfLevel)
                || !optionalLevel(QStringLiteral("mnLevel"), slice.mnLevel)) { return false; }
        }
        if (entry.contains(QStringLiteral("wfmDeemphasisUs"))) {
            double microseconds = 0;
            if (!number(entry, QStringLiteral("wfmDeemphasisUs"), 50, 75, microseconds, true)
                || (microseconds != 50 && microseconds != 75)) { return false; }
            slice.wfmDeemphasisUs = static_cast<int>(microseconds);
        }
        if (entry.contains(QStringLiteral("wfmForceMono"))) {
            if (!entry.value(QStringLiteral("wfmForceMono")).isBool()) { return false; }
            slice.wfmForceMono = entry.value(QStringLiteral("wfmForceMono")).toBool();
        }
        const QJsonObject squelch = entry.value(QStringLiteral("squelch")).toObject();
        if (!squelch.value(QStringLiteral("enabled")).isBool()
            || !percent(squelch, QStringLiteral("level"), slice.squelchLevel)) {
            return false;
        }
        slice.squelchEnabled = squelch.value(QStringLiteral("enabled")).toBool();
        if (squelch.contains(QStringLiteral("automatic"))) {
            if (!squelch.value(QStringLiteral("automatic")).isBool()) { return false; }
            slice.automaticSquelch = squelch.value(QStringLiteral("automatic")).toBool();
        }
        if (squelch.contains(QStringLiteral("marginDb"))) {
            double margin = 0;
            if (!number(squelch, QStringLiteral("marginDb"), 5, 20, margin, true)) { return false; }
            slice.automaticSquelchMarginDb = static_cast<int>(margin);
        }
        if (slice.automaticSquelch && !slice.squelchEnabled) { return false; }
        slice.audioMute = entry.value(QStringLiteral("audioMute")).toBool();
        parsed.slices.insert(slice.id, slice);
    }
    if (object.contains(QStringLiteral("trackingNotches"))) {
        if (!object.value(QStringLiteral("trackingNotches")).isObject()) { return false; }
        const auto notches = object.value(QStringLiteral("trackingNotches")).toObject();
        if (!notches.value(QStringLiteral("enabled")).isBool()
            || !notches.value(QStringLiteral("entries")).isObject()) { return false; }
        parsed.trackingNotches.enabled = notches.value(QStringLiteral("enabled")).toBool();
        const auto entries = notches.value(QStringLiteral("entries")).toObject();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            bool ok = false;
            rtl::RtlTrackingNotch notch;
            notch.id = it.key().toInt(&ok);
            if (!ok || notch.id < 0 || QString::number(notch.id) != it.key() || !it.value().isObject()) { return false; }
            const auto entry = it.value().toObject();
            if (!number(entry, QStringLiteral("centerHz"), 24000, 1766000000, notch.centerHz)
                || !number(entry, QStringLiteral("widthHz"), 200, 12000, notch.widthHz)
                || !entry.value(QStringLiteral("active")).isBool()) { return false; }
            notch.active = entry.value(QStringLiteral("active")).toBool();
            parsed.trackingNotches.entries.push_back(notch);
        }
        std::ranges::sort(parsed.trackingNotches.entries, {}, &rtl::RtlTrackingNotch::id);
        if (!parsed.trackingNotches.valid()) { return false; }
    }
    output = std::move(parsed); // Never expose a partially parsed document.
    reason.clear();
    return true;
}

RtlSliceSettings::ReadResult RtlSliceSettings::load() const
{
    ReadResult result;
    if (m_scope.family() != QLatin1String("rtl")) {
        result.status = ReadStatus::Refused;
        result.reason = QStringLiteral("invalid RTL scope");
        return result;
    }
    int version = 0;
    Status status;
    QJsonObject object = m_scope.featureExact(featureName(), &version, &status);
    if (status == Status::Missing && !m_scope.radioId().isEmpty()) {
        object = RadioSettingsScope(m_scope.family(), {}).featureExact(featureName(), &version, &status);
    }
    if (status == Status::Missing) {
        return result;
    }
    result.status = ReadStatus::Refused;
    if (status != Status::Present || version != kSchemaVersion) {
        result.reason = QStringLiteral("unreadable or unsupported schema; preserving stored state");
    } else if (decode(object, result.document, result.reason)) {
        result.status = ReadStatus::Ready;
    }
    if (result.status == ReadStatus::Refused) {
        refuse(result.reason);
    }
    return result;
}

bool RtlSliceSettings::patch(double captureCenterHz, double sampleRateHz,
                             const QVector<Slice>& accepted, const QVector<int>& removed,
                             const std::optional<rtl::RtlTrackingNotches>& trackingNotches) const
{
    if (m_scope.family() != QLatin1String("rtl")) {
        return refuse(QStringLiteral("invalid RTL write scope"));
    }
    if (accepted.size() > static_cast<qsizetype>(SharedCapturePolicy::kMaxEntries)
        || removed.size() > static_cast<qsizetype>(SharedCapturePolicy::kMaxEntries)) {
        return refuse(QStringLiteral("too many patch entries"));
    }
    int version = 0;
    Status status;
    QJsonObject object = m_scope.featureExact(featureName(), &version, &status);
    // Decode only as a validity gate: patch the original JSON below so unknown
    // members survive; rebuilding it from the typed document would lose them.
    Document previous;
    QString reason;
    if (status == Status::Unavailable || status == Status::Corrupt
        || (status == Status::Present && version != kSchemaVersion)
        || !decode(object, previous, reason)) {
        return refuse(QStringLiteral("refusing mutation over unreadable/newer document"));
    }
    QJsonObject slices = object.value(QStringLiteral("slices")).toObject();
    QSet<int> seen;
    for (const Slice& slice : accepted) {
        if (seen.contains(slice.id)) {
            return refuse(QStringLiteral("duplicate accepted stable ID"));
        }
        seen.insert(slice.id);
        const QString key = QString::number(slice.id);
        slices.insert(key, encodeSlice(slice, slices.value(key).toObject()));
    }
    for (int id : removed) {
        if (id < 0 || id >= static_cast<int>(SharedCapturePolicy::kMaxEntries)
            || seen.contains(id)) {
            return refuse(QStringLiteral("invalid or conflicting removed stable ID"));
        }
        seen.insert(id);
        slices.remove(QString::number(id));
    }
    object.insert(QStringLiteral("captureCenterHz"), captureCenterHz);
    object.insert(QStringLiteral("sampleRateHz"), sampleRateHz);
    object.insert(QStringLiteral("slices"), slices);
    // An absent bank already means enabled with no entries. A reconnect that
    // merely observes that default must not rewrite the saved document.
    if (trackingNotches && (object.contains(QStringLiteral("trackingNotches"))
        || !trackingNotches->enabled || !trackingNotches->entries.empty())) {
        if (!trackingNotches->valid()) { return refuse(QStringLiteral("invalid tracking notch bank")); }
        auto notches = object.value(QStringLiteral("trackingNotches")).toObject();
        const auto previousEntries = notches.value(QStringLiteral("entries")).toObject();
        QJsonObject entries;
        for (const auto& notch : trackingNotches->entries) {
            const QString key = QString::number(notch.id);
            auto entry = previousEntries.value(key).toObject();
            entry.insert(QStringLiteral("centerHz"), notch.centerHz);
            entry.insert(QStringLiteral("widthHz"), notch.widthHz);
            entry.insert(QStringLiteral("active"), notch.active);
            entries.insert(key, entry);
        }
        notches.insert(QStringLiteral("enabled"), trackingNotches->enabled);
        notches.insert(QStringLiteral("entries"), entries);
        object.insert(QStringLiteral("trackingNotches"), notches);
    }
    Document checked;
    if (!decode(object, checked, reason)) {
        return refuse(reason);
    }
    if (!m_scope.setFeature(featureName(), kSchemaVersion, object)) {
        return refuse(QStringLiteral("atomic write refused; retry remains possible"));
    }
    return true;
}

RtlSliceSettings::MigrationResult RtlSliceSettings::migrateLegacy(
    const RadioSerialIdentity& identity) const
{
    if (m_scope.family() != QLatin1String("rtl")
        || m_scope.radioId() != settingsRadioId(m_scope.family(), {}, identity)) {
        refuse(QStringLiteral("migration scope does not match reported identity"));
        return MigrationResult::Retry;
    }
    int version = 0;
    Status status;
    QJsonObject existing = m_scope.featureExact(featureName(), &version, &status);
    if (status == Status::Missing && !m_scope.radioId().isEmpty()) {
        existing = RadioSettingsScope(m_scope.family(), {}).featureExact(featureName(), &version, &status);
    }
    // An effective new-format family document (including empty) already owns
    // defaults. Never resurrect legacy per-radio data over that operator choice.
    if (status == Status::Present) {
        Document checked;
        QString reason;
        if (version != kSchemaVersion || !decode(existing, checked, reason)) {
            refuse(QStringLiteral("existing migration destination is invalid/newer"));
            return MigrationResult::Retry;
        }
        return MigrationResult::Settled; // Includes deliberately empty documents.
    }
    if (status != Status::Missing) {
        refuse(QStringLiteral("migration destination unavailable or corrupt"));
        return MigrationResult::Retry;
    }
    // Old RTL rows used this exact spelling for enumeration fallbacks. Even
    // a genuine serial with that spelling today cannot establish their owner.
    if (m_scope.radioId().startsWith(QLatin1String("rtl:"))) {
        bool indexOk = false;
        const int oldIndex = m_scope.radioId().mid(4).toInt(&indexOk);
        if (indexOk && oldIndex >= 0) {
            refuse(QStringLiteral("ambiguous index-keyed legacy scope cannot be claimed"));
            return MigrationResult::Retry;
        }
    }
    const RadioStateMemory::RtlMigrationSource source = RadioStateMemory::rtlMigrationSource(m_scope);
    if (source.status == Status::Missing) {
        return MigrationResult::NoSource;
    }
    if (source.status != Status::Present) {
        refuse(QStringLiteral("legacy source unreadable or unsupported; retry later"));
        return MigrationResult::Retry;
    }
    Slice slice;
    slice.id = 0;
    slice.frequencyHz = source.state.rfFrequencyHz;
    slice.mode = source.state.mode;
    slice.filterLowHz = source.state.filterLowHz;
    slice.filterHighHz = source.state.filterHighHz;
    return patch(source.state.rfFrequencyHz, source.state.sampleRateHz, {slice})
        ? MigrationResult::Claimed : MigrationResult::Retry;
}

SharedCapturePolicy::RestoreResult RtlSliceSettings::planRestore(
    const Document& saved, const SharedCapturePolicy::CaptureDescriptor& actual,
    std::span<const SharedCapturePolicy::SliceDescriptor> prepared,
    std::span<const SharedCapturePolicy::CenterDomain> domains,
    SharedCapturePolicy::ReceiverLimits limits)
{
    SharedCapturePolicy::RestoreResult invalid;
    invalid.error = SharedCapturePolicy::Error::InvalidIdentity;
    if (prepared.size() > SharedCapturePolicy::kMaxEntries
        || prepared.size() != static_cast<std::size_t>(saved.slices.size())) {
        return invalid;
    }
    QSet<int> seen;
    for (const SharedCapturePolicy::SliceDescriptor& descriptor : prepared) {
        const auto it = saved.slices.constFind(descriptor.stableId);
        if (it == saved.slices.cend() || seen.contains(descriptor.stableId)
            || descriptor.carrierHz != it->frequencyHz
            || descriptor.filterLowHz != it->filterLowHz
            || descriptor.filterHighHz != it->filterHighHz) {
            return invalid;
        }
        seen.insert(descriptor.stableId);
    }
    SharedCapturePolicy::RestoreResult result =
        SharedCapturePolicy::restoreFixedCapture(actual, prepared, domains, limits);
    for (const SharedCapturePolicy::RestoreRejection& rejection : result.rejected) {
        qWarning() << "RtlSliceSettings: restore rejected slice" << rejection.stableId
                   << "policy reason" << static_cast<int>(rejection.reason);
    }
    return result;
}

} // namespace AetherSDR
