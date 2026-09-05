#include "core/backends/hl2/Hl2FilterBoardSettings.h"

#include "core/RadioSettingsScope.h"

#include <QDebug>
#include <QJsonObject>

namespace AetherSDR {

namespace {
constexpr const char* kFieldManualEnabled = "manualEnabled";
constexpr const char* kFieldBands = "bands";
constexpr const char* kFieldRx = "rx";
constexpr const char* kFieldTx = "tx";
}  // namespace

bool Hl2FilterBoardSettings::manualEnabled(const RadioSettingsScope& scope)
{
    return scope.feature(kFeature).value(QLatin1String(kFieldManualEnabled)).toBool(false);
}

void Hl2FilterBoardSettings::setManualEnabled(const RadioSettingsScope& scope, bool enabled)
{
    // Read-modify-write against the EXACT row — see Hl2MiscOptionsSettings'
    // writeField() for why (AGENTS.md "Radio-Scoped Feature Documents").
    QJsonObject doc = scope.featureExact(kFeature);
    doc[QLatin1String(kFieldManualEnabled)] = enabled;
    if (!scope.setFeature(kFeature, kSchemaVersion, doc)) {
        qWarning() << "Hl2FilterBoardSettings: manualEnabled write did not persist —"
                   << "no radio identity yet, or the store refused it";
    }
}

hl2::ManualFilterTable Hl2FilterBoardSettings::table(const RadioSettingsScope& scope)
{
    hl2::ManualFilterTable out;
    const QJsonObject bands = scope.feature(kFeature).value(QLatin1String(kFieldBands)).toObject();
    for (auto it = bands.constBegin(); it != bands.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        // Validated against the field's own range rather than trusted, so a
        // hand-edited or truncated document cannot command a nonsense relay
        // pattern (Principle VII) — masked to 7 bits exactly like
        // manualFilterByte() masks a live lookup.
        const int rx = entry.value(QLatin1String(kFieldRx)).toInt(0) & 0x7F;
        const int tx = entry.value(QLatin1String(kFieldTx)).toInt(0) & 0x7F;
        out.insert(it.key(), hl2::ManualFilterBand{static_cast<std::uint8_t>(rx),
                                                    static_cast<std::uint8_t>(tx)});
    }
    return out;
}

void Hl2FilterBoardSettings::setTable(const RadioSettingsScope& scope,
                                      const hl2::ManualFilterTable& table)
{
    QJsonObject bands;
    for (auto it = table.constBegin(); it != table.constEnd(); ++it) {
        QJsonObject entry;
        entry[QLatin1String(kFieldRx)] = static_cast<int>(it->rxMask);
        entry[QLatin1String(kFieldTx)] = static_cast<int>(it->txMask);
        bands[it.key()] = entry;
    }
    QJsonObject doc = scope.featureExact(kFeature);
    doc[QLatin1String(kFieldBands)] = bands;
    if (!scope.setFeature(kFeature, kSchemaVersion, doc)) {
        qWarning() << "Hl2FilterBoardSettings: table write did not persist —"
                   << "no radio identity yet, or the store refused it";
    }
}

}  // namespace AetherSDR
