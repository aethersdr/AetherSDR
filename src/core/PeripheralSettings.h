#pragma once

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <optional>

namespace AetherSDR {

// Peripherals settings in one nested AppSettings JSON blob; the old flat key from
// #3321 is migrated. The blob holds the Setup Peripherals list (VisibleDevices),
// the global AutoReconnect, per-device preferences ("Connect automatically",
// obj[<id>]["AutoConnect"]), and the ACOM and SPE Expert manual-connection
// settings (ManualIp/ManualPort/SerialPort/ConnectionMode), which nest under
// obj["Acom"] / obj["SpeExpert"]. The TGXL/PGXL/Antenna Genius/ShackSwitch
// endpoints stay in their own flat keys; only their list membership and
// AutoConnect preference live here.
class PeripheralSettings {
public:
    // VisibleDevices uses stable lowercase UI identifiers, not the legacy
    // connection-object names (Acom/SpeExpert/Vkamp/Lp100a). Keep these namespaces
    // distinct: changing their spelling would require a settings migration.
    // nullopt means this installation predates the list UI: the dialog can
    // seed it from already configured manual targets without losing them.
    static std::optional<QStringList> visibleDeviceIds()
    {
        const QJsonValue value = readObj().value(QStringLiteral("VisibleDevices"));
        if (!value.isArray()) {
            return std::nullopt;
        }
        QStringList ids;
        for (const QJsonValue& entry : value.toArray()) {
            if (entry.isString() && !ids.contains(entry.toString())) {
                ids.append(entry.toString());
            }
        }
        return ids;
    }

    static void setVisibleDeviceIds(const QStringList& ids)
    {
        QJsonObject root = readObj();
        QJsonArray value;
        for (const QString& id : ids) {
            if (!id.isEmpty() && !value.contains(id)) {
                value.append(id);
            }
        }
        root[QStringLiteral("VisibleDevices")] = value;
        write(root);
    }

    // "Connect automatically" for the network devices tgxl, pgxl, ag and
    // shackswitch. Default on: with no key stored, startup, discovery, alternate
    // and reconnect attempts behave as they always have. Off blocks every
    // automatic attempt for that device; an explicit Connect still works and
    // never changes this value.
    static bool autoConnect(const QString& id)
    {
        const QJsonValue value = deviceObj(id).value(QStringLiteral("AutoConnect"));
        if (value.isBool()) {
            return value.toBool();
        }
        if (value.isString()) {
            return value.toString().compare(QStringLiteral("False"), Qt::CaseInsensitive) != 0;
        }
        return true;
    }

    static void setAutoConnect(const QString& id, bool on)
    {
        setDeviceField(id, QStringLiteral("AutoConnect"),
                       QJsonValue(on ? QStringLiteral("True") : QStringLiteral("False")));
        clearDeviceField(id, QStringLiteral("DiscoveryDismissed"));
    }

    // Remove returns the device to its default: the toggle is cleared, so a row
    // that is no longer listed never carries a hidden "do not connect". The
    // retired DiscoveryDismissed key (branch builds only) is dropped with it.
    static void resetAutoConnect(const QString& id)
    {
        clearDeviceField(id, QStringLiteral("AutoConnect"));
        clearDeviceField(id, QStringLiteral("DiscoveryDismissed"));
    }

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
    // "Peripherals" root object. Used by the ACOM and SPE Expert rows so far
    // (see the class comment above); kept generic by device/field name rather
    // than ACOM-specific so a future migration of the other peripheral rows
    // can reuse the same accessors instead of re-inventing them.
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

    static void clearDeviceConnection(const QString& device)
    {
        QJsonObject root = readObj();
        if (!root.contains(device)) {
            return;
        }
        QJsonObject connection = root.value(device).toObject();
        for (const QString& field : {QStringLiteral("ConnectionMode"),
                                     QStringLiteral("ManualIp"),
                                     QStringLiteral("ManualPort"),
                                     QStringLiteral("SerialPort")}) {
            connection.remove(field);
        }
        if (connection.isEmpty()) {
            root.remove(device);
        } else {
            root[device] = connection;
        }
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
