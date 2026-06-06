#pragma once

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// Persistence helper for display-related UI toggles (#3283 Lean Mode is the
// first; future display-feature toggles like frameless / theme variants land
// here as additional fields).
//
// Stored as a nested JSON blob under AppSettings["Display"], per the
// nested-JSON-per-feature convention (constitution Principle V).  The legacy
// flat key "LeanMode" is migrated into this blob on first read so existing
// users keep their behavior.
class DisplaySettings {
public:
    static bool leanMode() { return readObj().value("leanMode").toString("False") == "True"; }

    static void setLeanMode(bool on)
    {
        QJsonObject o = readObj();
        o["leanMode"] = on ? QStringLiteral("True") : QStringLiteral("False");
        write(o);
    }

    // One-shot migration from the legacy "LeanMode" flat key.  Run at app
    // startup before any caller reads the new blob.  Safe to call repeatedly:
    // returns immediately if the new blob already exists.
    static void migrateLegacy()
    {
        auto& s = AppSettings::instance();
        if (s.contains("Display")) return;
        const bool legacyLean =
            s.value("LeanMode", "False").toString() == "True";
        QJsonObject o;
        o["leanMode"] = legacyLean ? QStringLiteral("True") : QStringLiteral("False");
        write(o);
        // Leave the legacy flat key in place — harmless after migration, and a
        // future cleanup PR can drop it once we're confident no other reader
        // still touches it.
    }

private:
    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value("Display", QString{}).toString();
        if (json.isEmpty()) return {};
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    static void write(const QJsonObject& o)
    {
        auto& s = AppSettings::instance();
        s.setValue("Display",
                   QString::fromUtf8(
                       QJsonDocument(o).toJson(QJsonDocument::Compact)));
        s.save();
    }
};

} // namespace AetherSDR
