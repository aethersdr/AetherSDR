#include "RtlSliceSettings.h"

#include "RadioStateMemory.h"

#include <QDebug>
#include <QSet>

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
    previous.insert(QStringLiteral("agcMode"), slice.agcMode);
    previous.insert(QStringLiteral("agcThreshold"), slice.agcThreshold);
    QJsonObject squelch = previous.value(QStringLiteral("squelch")).toObject();
    squelch.insert(QStringLiteral("enabled"), slice.squelchEnabled);
    squelch.insert(QStringLiteral("level"), slice.squelchLevel);
    previous.insert(QStringLiteral("squelch"), squelch);
    previous.insert(QStringLiteral("audioGain"), slice.audioGain);
    previous.insert(QStringLiteral("audioMute"), slice.audioMute);
    previous.insert(QStringLiteral("audioPan"), slice.audioPan);
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
        const QJsonObject squelch = entry.value(QStringLiteral("squelch")).toObject();
        if (!squelch.value(QStringLiteral("enabled")).isBool()
            || !percent(squelch, QStringLiteral("level"), slice.squelchLevel)) {
            return false;
        }
        slice.squelchEnabled = squelch.value(QStringLiteral("enabled")).toBool();
        slice.audioMute = entry.value(QStringLiteral("audioMute")).toBool();
        parsed.slices.insert(slice.id, slice);
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
                             const QVector<Slice>& accepted, const QVector<int>& removed) const
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
