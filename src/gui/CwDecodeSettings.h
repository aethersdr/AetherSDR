#pragma once

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace MasterSDR {

// Persistence helper for the two independent CW decode toggles (#2417).
//
// Stored as a nested JSON blob under AppSettings["CwDecoder"], per the
// nested-JSON-per-feature convention (constitution Principle V).  The
// legacy flat key "CwDecodeOverlay" is migrated into this blob on first
// read so existing users keep their behavior — its old value becomes the
// initial value of the rx toggle.
class CwDecodeSettings {
public:
    static bool rxEnabled() { return readObj().value("rx").toString("True") == "True"; }
    static bool txEnabled() { return readObj().value("tx").toString("False") == "True"; }

    static void setRxEnabled(bool on)
    {
        QJsonObject o = readObj();
        o["rx"] = on ? QStringLiteral("True") : QStringLiteral("False");
        if (!o.contains("tx"))
            o["tx"] = QStringLiteral("False");
        write(o);
    }
    static void setTxEnabled(bool on)
    {
        QJsonObject o = readObj();
        o["tx"] = on ? QStringLiteral("True") : QStringLiteral("False");
        if (!o.contains("rx"))
            o["rx"] = QStringLiteral("True");
        write(o);
    }

    // True if either toggle is on — used to decide if the CW text panel
    // should be visible at all (regardless of which direction is fed).
    static bool anyEnabled() { return rxEnabled() || txEnabled(); }

    // One-shot migration from the legacy "CwDecodeOverlay" flat key.  Run
    // at startup before any caller reads the new blob.  Safe to call
    // repeatedly: returns immediately if the new blob already exists.
    static void migrateLegacy()
    {
        auto& s = AppSettings::instance();
        if (s.contains("CwDecoder")) return;
        const bool legacyRx =
            s.value("CwDecodeOverlay", "True").toString() == "True";
        QJsonObject o;
        o["rx"] = legacyRx ? QStringLiteral("True") : QStringLiteral("False");
        o["tx"] = QStringLiteral("False");
        write(o);
    }

private:
    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value("CwDecoder", QString{}).toString();
        if (json.isEmpty()) return {};
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    static void write(const QJsonObject& o)
    {
        auto& s = AppSettings::instance();
        s.setValue("CwDecoder",
                   QString::fromUtf8(
                       QJsonDocument(o).toJson(QJsonDocument::Compact)));
        s.save();
    }
};

} // namespace MasterSDR
