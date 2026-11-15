#pragma once

// Client-side voice keyer persistence (RFC #4214). The configuration lives as
// ONE nested JSON blob under the single AppSettings key "VoiceKeyer" — never
// flat keys. These labels are this client's own; they
// never issue a `dvk` command, so renaming a local slot cannot touch a radio's
// stored DVK names.

#include "models/VoiceKeyerSource.h"

#include <QJsonObject>
#include <QString>

namespace AetherSDR {

class VoiceKeyerSettings {
public:
    // Radio vs Local. Auto until the operator chooses.
    static VoiceKeyerSourceSetting source();
    static void setSource(VoiceKeyerSourceSetting setting);

    // Local slot labels, 1-based slot ids. Empty when the slot has no label.
    static QString slotName(int id);
    static void setSlotName(int id, const QString& name);

    // Where local recordings live: ~/Documents/AetherSDR/VoiceKeyer, next to
    // QsoRecorder's ~/Documents/AetherSDR/Recordings, so operators can find,
    // copy and drop in WAVs.
    static QString recordingsDir();

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

} // namespace AetherSDR
