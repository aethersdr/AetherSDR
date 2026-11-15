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
// restores it instead; see voxHoldForRecording() and the controller's guards.
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

// What recording must do about VOX before it starts.
//
// Only a connected radio with a VOX AetherSDR controls can be keyed by it, so
// a stale flag on a disconnected radio, or on one with no VOX at all (the
// Hermes-Lite 2), is not a reason to touch anything.
//
// HoldAndRestore: VOX is on, or a restore is on its way back (the model reads
// off only because the radio has not answered yet) — turn it off for the
// recording and give it back afterwards.
//
// ConfirmOff: the model says off, but only because it was ASKED to be — an
// operator's VOX click writes the model before the radio answers, and the
// radio may answer late or refuse. Ask again and wait for the
// radio's own answer; it was meant to be off, so nothing is given back.
enum class VoxHold { None, ConfirmOff, HoldAndRestore };

inline VoxHold voxHoldForRecording(bool radioConnected, bool radioHasVox, bool voxEnabled,
                                   bool voxRestorePending, bool voxStateConfirmed)
{
    if (!radioConnected || !radioHasVox)
        return VoxHold::None;
    if (voxEnabled || voxRestorePending)
        return VoxHold::HoldAndRestore;
    return voxStateConfirmed ? VoxHold::None : VoxHold::ConfirmOff;
}

}  // namespace AetherSDR
