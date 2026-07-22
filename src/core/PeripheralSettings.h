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
//
// ACOM's own manual connection settings (ManualIp/ManualPort/SerialPort/
// ConnectionMode) use the generic per-device accessors below, nested under
// obj["Acom"] — ACOM has never shipped with flat keys in a released build,
// so there's no legacy data to migrate; new installs write directly into the
// nested shape. TGXL/PGXL/Antenna Genius/ShackSwitch still use their
// original flat AppSettings keys directly (not through this class) —
// migrating those is legitimate follow-up work, scoped to its own PR rather
// than bundled with the ACOM feature that motivated adding these accessors.
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

    // Generic per-device connection settings — e.g. device "Acom", field
    // "ManualIp" reads/writes obj["Acom"]["ManualIp"] within the shared
    // "Peripherals" root object. Currently only ACOM uses these (see the
    // class comment above); kept generic by device/field name rather than
    // ACOM-specific so a future migration of the other peripheral rows can
    // reuse the same accessors instead of re-inventing them.
    static QString deviceString(const QString& device, const QString& field,
                                const QString& def = QString())
    {
        const QJsonValue v = deviceObj(device).value(field);
        return v.isUndefined() || v.isNull() ? def : v.toString();
    }

    static void setDeviceString(const QString& device, const QString& field,
                                const QString& value)
    {
        setDeviceField(device, field, QJsonValue(value));
    }

    static int deviceInt(const QString& device, const QString& field, int def = 0)
    {
        const QJsonValue v = deviceObj(device).value(field);
        if (v.isDouble()) {
            return v.toInt();
        }
        if (v.isString()) {
            bool ok = false;
            const int n = v.toString().toInt(&ok);
            if (ok) {
                return n;
            }
        }
        return def;
    }

    static void setDeviceInt(const QString& device, const QString& field, int value)
    {
        setDeviceField(device, field, QJsonValue(value));
    }

    static void clearDeviceField(const QString& device, const QString& field)
    {
        QJsonObject root = readObj();
        if (!root.contains(device)) {
            return;
        }
        QJsonObject devObj = root.value(device).toObject();
        if (!devObj.contains(field)) {
            return;
        }
        devObj.remove(field);
        root[device] = devObj;
        write(root);
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

    static QJsonObject readRawObj()
    {
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

    static QJsonObject readObj()
    {
        migrateLegacy();
        return readRawObj();
    }

    static QJsonObject deviceObj(const QString& device)
    {
        return readObj().value(device).toObject();
    }

    static void setDeviceField(const QString& device, const QString& field,
                               const QJsonValue& value)
    {
        QJsonObject root = readObj();
        QJsonObject devObj = root.value(device).toObject();
        devObj[field] = value;
        root[device] = devObj;
        write(root);
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
