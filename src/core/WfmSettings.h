#pragma once
#include "core/AppSettings.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

namespace AetherSDR {

// WFM demodulator settings live in one nested AppSettings JSON blob per
// constitution Principle V.  The temporary flat key "WfmAudioDevice" from
// the initial PR is migrated on first access so existing testers keep their
// selected device.
class WfmSettings
{
public:
    static QString audioDeviceId()
    {
        return readObj().value(QStringLiteral("AudioDeviceId")).toString();
    }

    static void setAudioDeviceId(const QString& id)
    {
        QJsonObject obj = readObj();
        obj[QStringLiteral("AudioDeviceId")] = id;
        write(obj);
    }

    static void clearAudioDeviceId()
    {
        QJsonObject obj = readObj();
        obj.remove(QStringLiteral("AudioDeviceId"));
        write(obj);
    }

    static void migrateLegacy()
    {
        auto& settings = AppSettings::instance();
        constexpr const char* kLegacyKey = "WfmAudioDevice";
        if (!settings.contains(kLegacyKey))
            return;
        if (!settings.contains(kRootKey)) {
            const QString legacyId = settings.value(kLegacyKey).toString().trimmed();
            if (!legacyId.isEmpty()) {
                QJsonObject obj;
                obj[QStringLiteral("AudioDeviceId")] = legacyId;
                settings.setValue(kRootKey,
                    QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
            }
        }
        settings.remove(kLegacyKey);
        settings.save();
    }

private:
    static constexpr const char* kRootKey = "WFM";

    static QJsonObject readObj()
    {
        migrateLegacy();
        const QString json =
            AppSettings::instance().value(kRootKey, QString{}).toString();
        if (json.isEmpty())
            return {};
        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
            return {};
        return doc.object();
    }

    static void write(const QJsonObject& obj)
    {
        AppSettings::instance().setValue(kRootKey,
            QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
        AppSettings::instance().save();
    }
};

} // namespace AetherSDR
