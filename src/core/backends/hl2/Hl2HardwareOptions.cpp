#include "core/backends/hl2/Hl2HardwareOptions.h"

#include "core/RadioSettingsScope.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QtGlobal>

#include <cmath>

namespace AetherSDR {

namespace {
constexpr const char* kFieldCodec       = "codec";
constexpr const char* kFieldDither      = "ditherBit";
constexpr const char* kFieldRandom      = "randomBit";
constexpr const char* kFieldFilterBoard = "filterBoard";
constexpr const char* kFieldN2adrHpf    = "n2adrHpf";
constexpr const char* kFieldAtu         = "atuGateware";
constexpr const char* kFieldSpeaker     = "speakerLevelPercent";

// Read a bool that may legitimately be absent. `defaultValue` rather than false
// so a field added after a document was written keeps the field's own default
// instead of collapsing to zero — the difference matters for filterBoard's
// neighbours and it costs nothing to be consistent about it here.
bool boolField(const QJsonObject& doc, const char* key, bool defaultValue)
{
    const QJsonValue v = doc.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : defaultValue;
}

// Read an enum's backing int, distinguishing "absent" from "present and zero".
// Zero is a MEANINGFUL value for both enums here (Codec::None, FilterBoard::None)
// so toInt(default) alone would map an absent filterBoard onto "no board" and
// release the relays on every radio that predates this document.
int intField(const QJsonObject& doc, const char* key, int defaultValue)
{
    const QJsonValue v = doc.value(QLatin1String(key));
    if (!v.isDouble())
        return defaultValue;
    const double d = v.toDouble();
    if (!std::isfinite(d))
        return defaultValue;
    return static_cast<int>(std::llround(d));
}
}  // namespace

Hl2HardwareOptions Hl2HardwareOptions::load(const RadioSettingsScope& scope)
{
    Hl2HardwareOptions opts;      // every default already correct
    if (!scope.isValid())
        return opts;
    const QJsonObject doc = scope.feature(QLatin1String(kFeature));
    if (doc.isEmpty())
        return opts;
    opts.codec = clampCodec(intField(doc, kFieldCodec, static_cast<int>(opts.codec)));
    opts.ditherBit   = boolField(doc, kFieldDither, opts.ditherBit);
    opts.randomBit   = boolField(doc, kFieldRandom, opts.randomBit);
    opts.filterBoard = clampFilterBoard(
        intField(doc, kFieldFilterBoard, static_cast<int>(opts.filterBoard)));
    opts.n2adrHpf    = boolField(doc, kFieldN2adrHpf, opts.n2adrHpf);
    opts.atuGateware = boolField(doc, kFieldAtu, opts.atuGateware);
    opts.speakerLevelPercent = clampSpeakerLevel(
        intField(doc, kFieldSpeaker, opts.speakerLevelPercent));
    return opts;
}

void Hl2HardwareOptions::save(const RadioSettingsScope& scope,
                              const Hl2HardwareOptions& opts)
{
    if (!scope.isValid())
        return;
    // Read-modify-write, so a field this build does not know about survives a
    // change made from this build (Principle XIV — persisted as a unit).
    int storedVersion = 0;
    AppSettings::FeatureReadStatus status = AppSettings::FeatureReadStatus::Unavailable;
    QJsonObject doc = scope.featureExact(QLatin1String(kFeature), &storedVersion, &status);
    if (status == AppSettings::FeatureReadStatus::Corrupt
        || status == AppSettings::FeatureReadStatus::Unavailable
        || storedVersion > kSchemaVersion) {
        qWarning("Hl2HardwareOptions: refusing to overwrite unreadable or newer hardware options");
        return;
    }
    doc[QLatin1String(kFieldCodec)]       = static_cast<int>(opts.codec);
    doc[QLatin1String(kFieldDither)]      = opts.ditherBit;
    doc[QLatin1String(kFieldRandom)]      = opts.randomBit;
    doc[QLatin1String(kFieldFilterBoard)] = static_cast<int>(opts.filterBoard);
    doc[QLatin1String(kFieldN2adrHpf)]    = opts.n2adrHpf;
    doc[QLatin1String(kFieldAtu)]         = opts.atuGateware;
    doc[QLatin1String(kFieldSpeaker)]     = clampSpeakerLevel(opts.speakerLevelPercent);
    // Checked, like Hl2FreqCal::savePpb: setFeature() refuses while the store is
    // not ReadyToSave, and nothing reads these back off the radio — an operator
    // would only discover a silent failure at the next connect, when the codec
    // they configured is gone again.
    if (!scope.setFeature(QLatin1String(kFeature), kSchemaVersion, doc))
        qWarning("Hl2HardwareOptions: hardware options did not persist");
    AppSettings::instance().save();
}

}  // namespace AetherSDR
