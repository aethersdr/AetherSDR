#pragma once

#include "RadioSettingsScope.h"

#include <QDebug>
#include <QJsonObject>
#include <QString>

namespace AetherSDR::CwxMacroTemplateSettings {

// The typed text of each CWX macro that holds {CALL}: one document per radio,
// (family, serial, "CwxMacroTemplates") in radio_settings. The radio's slot
// holds the text without the placeholder, so this is the only copy of where
// {CALL} sits. Never written back to the radio: CwxModel lays it over the
// radio's text only while the slot still equals its portable form.
inline constexpr int kSchemaVersion = 1;
inline QString featureName() { return QStringLiteral("CwxMacroTemplates"); }
inline QString slotsField() { return QStringLiteral("slots"); }
inline QString slotKey(int idx) { return QString::number(idx + 1); }   // "1".."12", as F1..F12

// The saved text for macro idx (0-based), or empty. Exact radio only: a macro
// slot belongs to one physical radio, so there is no family-wide fallback.
inline QString load(const RadioSettingsScope& scope, int idx)
{
    if (!scope.isValid() || scope.radioId().isEmpty() || idx < 0 || idx >= 12) { return {}; }
    return scope.featureExact(featureName()).value(slotsField()).toObject()
        .value(slotKey(idx)).toString();
}

// Saves the typed text for macro idx; an empty text removes the entry. False
// when the write did not persist.
inline bool save(const RadioSettingsScope& scope, int idx, const QString& typed)
{
    if (!scope.isValid() || scope.radioId().isEmpty() || idx < 0 || idx >= 12) { return false; }
    int storedSchema = 0;
    QJsonObject doc = scope.featureExact(featureName(), &storedSchema);
    if (storedSchema > kSchemaVersion) {
        qWarning() << "CwxMacroTemplates: stored schema" << storedSchema
                   << "is newer than" << kSchemaVersion << "- not written";
        return false;
    }
    QJsonObject slotTexts = doc.value(slotsField()).toObject();
    const QString key = slotKey(idx);
    if (slotTexts.value(key).toString() == typed) { return true; }   // also: nothing saved, nothing to remove
    if (typed.isEmpty()) { slotTexts.remove(key); }
    else { slotTexts.insert(key, typed); }
    const bool ok = slotTexts.isEmpty()
        ? scope.removeFeature(featureName())
        : scope.setFeature(featureName(), kSchemaVersion, QJsonObject{{slotsField(), slotTexts}});
    if (!ok) {
        qWarning() << "CwxMacroTemplates: settings write did not persist for macro" << key;
    }
    return ok;
}

} // namespace AetherSDR::CwxMacroTemplateSettings
