#pragma once

#include <QLatin1String>
#include <QString>

#include "core/AppSettings.h"

// Which stored level the master slider is moving, and where a profile with no
// stored level starts it.
//
// One slider drives two different outputs: this computer's sink when PC Audio
// is on, and the RADIO's own output when it is off (MainWindow::
// applyMasterVolume -> RadioModel::setLineoutGain). On a radio whose receive
// audio THIS CLIENT originates, they also shared one stored number, so a level
// set for the radio's speaker came back as the headphone level on the next
// launch. Two outputs, two levels -- but only where the second level is ours
// to keep; see separatesRadioOutputLevel().
//
// THE RADIO-SIDE DEFAULT IS 0, NOT 100, and only on those radios. A client that
// demodulates on this host originates the radio's audio itself, and a G2 has no
// output-level register to turn it down with, so "no stored preference" has to
// mean silence the operator raises deliberately rather than full scale into a
// speaker in the room. There is deliberately NO migration from the shared key:
// seeding the new level from the old one would reproduce the very startup level
// this exists to fix.
//
// Pure selectors so audio_output_volume_policy_test can pin them against the
// readers without a settings file; the callers do the AppSettings I/O.

namespace AetherSDR::AudioOutputVolumePolicy {

inline QString pcAudioKey()      { return QStringLiteral("MasterVolume"); }
inline QString radioOutputKey()  { return QStringLiteral("RadioSpeakerVolume"); }

// Defaults as AppSettings holds them -- strings, because value() takes its
// fallback as one and a mismatched type here would only show on a fresh
// profile, which is exactly the first-launch case this is for.
inline QString pcAudioDefault()     { return QStringLiteral("100"); }
inline QString radioOutputDefault() { return QStringLiteral("0"); }

// Which families keep the radio's output level separately from the PC sink's.
//
// ANAN only. The condition is not "which vendor" but "who owns the level": this
// client originates that radio's audio and the radio has no register to attenuate
// it with, so remembering it is ours to do. A radio that owns its own mixer --
// anything with a command plane reporting a level back -- already keeps it, and
// splitting it here would mean the client writing over what was set at the radio.
// Everything else keeps the single historical key and its default, so this change
// is invisible on those families.
inline bool separatesRadioOutputLevel(const QString& family)
{
    return family.compare(QLatin1String("anan"), Qt::CaseInsensitive) == 0;
}

// The key, and its default, for the sink actually carrying receive audio.
// Falls back to the historical pair whenever the split does not apply, so a
// caller that cannot name a family (or has no radio yet) gets today's behaviour.
inline QString volumeKey(bool pcAudioEnabled, const QString& family = QString())
{
    return (!pcAudioEnabled && separatesRadioOutputLevel(family)) ? radioOutputKey()
                                                                  : pcAudioKey();
}

inline QString volumeDefault(bool pcAudioEnabled, const QString& family = QString())
{
    return (!pcAudioEnabled && separatesRadioOutputLevel(family)) ? radioOutputDefault()
                                                                  : pcAudioDefault();
}

// PC Audio as the title bar persists it. One spelling of this test, because it
// decides which level every caller below is reading.
inline bool pcAudioEnabled()
{
    return AppSettings::instance().value(QStringLiteral("PcAudioEnabled"),
                                         QStringLiteral("True")).toString()
           == QLatin1String("True");
}

// The level the active sink is at, for a caller that wants to nudge or report
// it. Every +/-5 control and every diagnostic goes through here so none of them
// can read one output's level and write the other's.
inline int storedVolumePercent(const QString& family = QString())
{
    const bool pc = pcAudioEnabled();
    return AppSettings::instance().value(volumeKey(pc, family),
                                         volumeDefault(pc, family)).toInt();
}

}  // namespace AetherSDR::AudioOutputVolumePolicy
