#pragma once

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

namespace AetherSDR {

// Peripherals feature settings live in one nested AppSettings JSON blob per
// constitution Principle V. The temporary flat key from PR #3321 is migrated
// so testers of the PR branch keep their selected behavior.
class PeripheralSettings {
public:
    static bool autoReconnect()
    {
        const QJsonObject obj = readObj();
        const QJsonValue value = obj.value(QStringLiteral("AutoReconnect"));
        if (value.isBool()) {
            return value.toBool();
        }
        return value.toString(QStringLiteral("False"))
            .compare(QStringLiteral("True"), Qt::CaseInsensitive) == 0;
    }

    static void setAutoReconnect(bool on)
    {
        QJsonObject obj = readObj();
        obj[QStringLiteral("AutoReconnect")] =
            on ? QStringLiteral("True") : QStringLiteral("False");
        write(obj);
    }

    static void migrateLegacy()
    {
        auto& settings = AppSettings::instance();
        constexpr const char* kLegacyKey = "Peripherals_AutoReconnect";
        if (!settings.contains(kLegacyKey)) {
            return;
        }

        if (!settings.contains(kRootKey)) {
            const bool legacyOn = settings.value(kLegacyKey, QStringLiteral("False"))
                .toString()
                .compare(QStringLiteral("True"), Qt::CaseInsensitive) == 0;
            QJsonObject obj;
            obj[QStringLiteral("AutoReconnect")] =
                legacyOn ? QStringLiteral("True") : QStringLiteral("False");
            settings.setValue(kRootKey,
                              QString::fromUtf8(
                                  QJsonDocument(obj).toJson(QJsonDocument::Compact)));
        }
        settings.remove(kLegacyKey);
        settings.save();
    }

private:
    static constexpr const char* kRootKey = "Peripherals";

    static QJsonObject readObj()
    {
        migrateLegacy();

        const QString json =
            AppSettings::instance().value(kRootKey, QString{}).toString();
        if (json.isEmpty()) {
            return {};
        }

        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            return {};
        }
        return doc.object();
    }

    static void write(const QJsonObject& obj)
    {
        auto& settings = AppSettings::instance();
        settings.setValue(kRootKey,
                          QString::fromUtf8(
                              QJsonDocument(obj).toJson(QJsonDocument::Compact)));
        settings.remove(QStringLiteral("Peripherals_AutoReconnect"));
        settings.save();
    }
};

} // namespace AetherSDR
