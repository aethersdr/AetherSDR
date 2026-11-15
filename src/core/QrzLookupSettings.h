#pragma once

#include "AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// Persistence for QRZ lookup: one JSON blob, AppSettings["QrzLookup"] =
// {"enabled": "True", "username": "..."}. The password lives in the OS
// keychain (CallsignLookupService, key "qrz_password"), never in settings.
class QrzLookupSettings {
public:
    static bool enabled()
    {
        return readObj().value("enabled").toString("True") == "True";
    }
    static QString username()
    {
        return readObj().value("username").toString();
    }

    static void setEnabled(bool on)
    {
        QJsonObject o = readObj();
        o["enabled"] = on ? QStringLiteral("True") : QStringLiteral("False");
        write(o);
    }
    static void setUsername(const QString& user)
    {
        QJsonObject o = readObj();
        o["username"] = user;
        if (!o.contains("enabled"))
            o["enabled"] = QStringLiteral("True");
        write(o);
    }

private:
    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value("QrzLookup", QString{}).toString();
        if (json.isEmpty()) return {};
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    static void write(const QJsonObject& o)
    {
        auto& s = AppSettings::instance();
        s.setValue("QrzLookup",
                   QString::fromUtf8(
                       QJsonDocument(o).toJson(QJsonDocument::Compact)));
        s.save();
    }
};

} // namespace AetherSDR
