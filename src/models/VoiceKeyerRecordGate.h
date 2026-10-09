#pragma once

#include <QLatin1String>
#include <QString>

namespace AetherSDR {

// Why the client-side voice keyer may not start recording, or an empty string
// when it may. Pure policy: the controller reads the radio and passes answers.
//
// VOX is deliberately NOT a refusal. It keys on the microphone, and recording
// is the operator speaking into it, so with VOX enabled the act of recording
// puts the radio on the air (measured on a FLEX-6600) — but refusing the
// recording does not prevent that, because VOX keys on the voice whether or not
// anything is being recorded. Recording holds VOX off for its duration and
// restores it instead; see shouldHoldVoxOff() and the controller's guards.
//
// What is left is the mic source. Recording taps what this computer captures,
// so on a radio that chooses between its own mic inputs the PC has to be the
// chosen one or the recording would be silence. A radio with no such choice
// takes transmit audio from this computer by definition, and there the reported
// source is irrelevant — it is the applet that writes "PC" into the model,
// and recording must not wait on that having happened.
inline QString voiceKeyerRecordRefusal(bool hasSelectableMicInputs,
                                       const QString& micSource)
{
    if (!hasSelectableMicInputs)
        return {};
    if (micSource.compare(QLatin1String("PC"), Qt::CaseInsensitive) == 0)
        return {};
    return QStringLiteral("Recording uses the PC microphone, but the mic source is %1 "
                          "— set the mic source to PC to record.")
        .arg(micSource.isEmpty() ? QStringLiteral("not set") : micSource);
}

// Whether recording must hold VOX off. Only a connected radio can be keyed, so
// a stale flag on a disconnected one is not a reason to touch anything.
inline bool shouldHoldVoxOff(bool radioConnected, bool voxEnabled)
{
    return radioConnected && voxEnabled;
}

}  // namespace AetherSDR
