#pragma once

// Where a microHAM TRIO's configuration is kept. The connection is
// Peripherals.trio.ManualIp and Peripherals.trio.ManualPort in the Peripherals
// settings blob, beside the row's own Peripherals.trio.AutoConnect. Which Trio
// radio port a radio sits on is one feature document in that radio's exact
// settings scope, so another radio never inherits it.

#include "core/AppSettings.h"
#include "core/PeripheralSettings.h"
#include "core/RadioSettingsScope.h"

#include <QDebug>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace AetherSDR {

struct MicrohamTrioSettings {
    // The Peripherals row id and the settings group.
    static constexpr const char* kPeripheralId = "trio";
    // The Trio's web interface. Its own menu can move it.
    static constexpr int kDefaultPort = 80;

    QString host;                 // empty: no TRIO configured
    int     port{kDefaultPort};

    bool isConfigured() const { return !host.isEmpty(); }

    static MicrohamTrioSettings load()
    {
        MicrohamTrioSettings out;
        const QString id = QLatin1String(kPeripheralId);
        out.host = PeripheralSettings::deviceString(id, QStringLiteral("ManualIp")).trimmed();
        const int port = PeripheralSettings::deviceInt(id, QStringLiteral("ManualPort"), kDefaultPort);
        out.port = port >= 1 && port <= 65535 ? port : kDefaultPort;
        return out;
    }

    // An unconfigured TRIO stores no address.
    void save() const
    {
        if (!isConfigured()) {
            remove();
            return;
        }
        const QString id = QLatin1String(kPeripheralId);
        PeripheralSettings::setDeviceString(id, QStringLiteral("ManualIp"), host);
        PeripheralSettings::setDeviceInt(id, QStringLiteral("ManualPort"), port);
    }

    // The address only; "Connect automatically" is the Peripherals row's to reset.
    static void remove()
    {
        PeripheralSettings::clearDeviceConnection(QLatin1String(kPeripheralId));
    }
};

// One radio's place on the TRIO, as the operator said it: on radio port
// `slotKey` ("radio2"), which carried `slotLabel` ("HL2") then, or not on the
// Trio at all. Read and written at the exact (family, radio) row only.
struct MicrohamTrioBinding {
    static constexpr const char* kFeature = "TrioBinding";
    static constexpr int kSchemaVersion = 1;

    enum class Stored {
        None,        // no document: the operator has not said
        NotOnTrio,   // the operator said this radio is not on the Trio
        Bound,
        Unreadable,  // corrupt, incomplete, or written by a newer version
    };

    Stored  stored{Stored::None};
    QString trioHost;      // the Trio this binding was made against
    QString slotKey;       // "radio2"
    QString slotLabel;     // "HL2": the label under that key when bound

    // A scope names one radio only with a radio id; the family row is not one.
    static bool scopeNamesOneRadio(const RadioSettingsScope& scope)
    {
        return scope.isValid() && !scope.radioId().isEmpty();
    }

    static MicrohamTrioBinding load(const RadioSettingsScope& scope)
    {
        MicrohamTrioBinding out;
        if (!scopeNamesOneRadio(scope)) {
            return out;
        }
        int version = 0;
        AppSettings::FeatureReadStatus status = AppSettings::FeatureReadStatus::Missing;
        const QJsonObject doc = scope.featureExact(QLatin1String(kFeature), &version, &status);
        if (status == AppSettings::FeatureReadStatus::Missing) {
            return out;
        }
        if (status != AppSettings::FeatureReadStatus::Present || version != kSchemaVersion) {
            out.stored = Stored::Unreadable;
            return out;
        }
        const QJsonValue onTrio = doc.value(QLatin1String("onTrio"));
        if (!onTrio.isBool()) {
            out.stored = Stored::Unreadable;
            return out;
        }
        if (!onTrio.toBool()) {
            out.stored = Stored::NotOnTrio;
            return out;
        }
        out.trioHost = doc.value(QLatin1String("trioHost")).toString();
        out.slotKey = doc.value(QLatin1String("slotKey")).toString();
        out.slotLabel = doc.value(QLatin1String("slotLabel")).toString();
        out.stored = out.trioHost.isEmpty() || out.slotKey.isEmpty() ? Stored::Unreadable
                                                                     : Stored::Bound;
        return out;
    }

    // Stores a Bound or NotOnTrio binding. False when the scope names no one
    // radio, the stored document is newer than this code, or the store refused.
    bool save(const RadioSettingsScope& scope) const
    {
        if (!scopeNamesOneRadio(scope)
            || (stored != Stored::Bound && stored != Stored::NotOnTrio)) {
            return false;
        }
        if (stored == Stored::Bound && (trioHost.isEmpty() || slotKey.isEmpty())) {
            return false;
        }
        int version = 0;
        AppSettings::FeatureReadStatus status = AppSettings::FeatureReadStatus::Missing;
        (void)scope.featureExact(QLatin1String(kFeature), &version, &status);
        if (version > kSchemaVersion || status == AppSettings::FeatureReadStatus::Unavailable) {
            qWarning() << "TrioBinding: refusing to replace newer or unavailable settings";
            return false;
        }
        QJsonObject doc;
        doc.insert(QLatin1String("onTrio"), stored == Stored::Bound);
        if (stored == Stored::Bound) {
            doc.insert(QLatin1String("trioHost"), trioHost);
            doc.insert(QLatin1String("slotKey"), slotKey);
            doc.insert(QLatin1String("slotLabel"), slotLabel);
        }
        if (!scope.setFeature(QLatin1String(kFeature), kSchemaVersion, doc)) {
            qWarning() << "TrioBinding: settings write did not persist";
            return false;
        }
        return true;
    }

    static bool remove(const RadioSettingsScope& scope)
    {
        if (!scopeNamesOneRadio(scope)) {
            return false;
        }
        if (!scope.removeFeature(QLatin1String(kFeature))) {
            qWarning() << "TrioBinding: settings removal did not persist";
            return false;
        }
        return true;
    }
};

} // namespace AetherSDR
