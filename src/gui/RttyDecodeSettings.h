#pragma once

#include "RttyDecoderSensitivity.h"
#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// RTTY decoder settings (#5353): one nested JSON blob under
// AppSettings["RttyDecoder"], written whole in one setValue()+save() so no
// reader sees half an update. Older Mark/Shift/Baud/Reverse stay flat keys
// until migrated together. `enabled` is the operator's "I want this pane",
// separate from mode availability (otherwise any slice switch, pan change or
// rtty_mark echo would reopen a dismissed pane). Defaults to True.
class RttyDecodeSettings {
public:
    static bool enabled() { return readObj().value("enabled").toString("True") == "True"; }

    static void setEnabled(bool on)
    {
        QJsonObject o = readObj();
        o["enabled"] = on ? QStringLiteral("True") : QStringLiteral("False");
        write(o);
    }

    // Decoded-character confidence filter, 0..100 (see RttyDecoderSensitivity.h
    // for the slider→threshold mapping).  Lives in the same object so a
    // sensitivity edit and an enable/disable never clobber each other.
    static int sensitivity()
    {
        const int v = readObj().value("sensitivity").toInt(kRttySensitivityDefault);
        return v < 0 ? 0 : (v > 100 ? 100 : v);
    }

    static void setSensitivity(int v)
    {
        QJsonObject o = readObj();
        o["sensitivity"] = v;
        write(o);
    }

private:
    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value("RttyDecoder", QString{}).toString();
        if (json.isEmpty()) return {};
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    static void write(const QJsonObject& o)
    {
        auto& s = AppSettings::instance();
        s.setValue("RttyDecoder",
                   QString::fromUtf8(
                       QJsonDocument(o).toJson(QJsonDocument::Compact)));
        s.save();
    }
};

} // namespace AetherSDR
