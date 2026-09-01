#pragma once

#include "RttyDecoderSensitivity.h"
#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// The RTTY decoder's owned configuration object (#5353).
//
// Stored as one nested JSON blob under AppSettings["RttyDecoder"], per the
// nested-JSON-per-feature convention (constitution Principle V), and written
// whole through a single setValue()+save() so a reader never observes a
// half-updated object (Principle XIV).  The pane's older Mark/Shift/Baud/
// Reverse keys stay grandfathered as flat keys until they are migrated as a
// unit.
//
// `enabled` is the operator's explicit "I want this pane" state, and it is
// deliberately NOT derived from the slice's mode.  Before this existed the
// pane's visibility was recomputed from `slice->mode() == "RTTY"` on every
// refresh, so dismissing it with the ✕ button lasted only until the next
// slice switch, active-pan change, or rtty_mark status echo — a band change
// alone was enough to bring it back, because the radio resets rtty_mark and
// SliceModel re-emits rttyMarkChanged.  "The decoder is available for this
// slice" and "the operator wants the window" are separate states; this field
// is the second one.  It defaults to True so behavior is unchanged for
// anyone who never closes the pane.
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
