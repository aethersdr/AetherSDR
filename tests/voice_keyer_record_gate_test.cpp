// Why the client-side voice keyer may refuse to start recording.
//
// The guard that matters most: VOX. Recording is the operator speaking into the
// microphone, and VOX keys on the microphone, so with VOX enabled the act of
// recording puts the radio on the air — measured on a FLEX-6600. Refusing the
// recording does not prevent that, because VOX keys on the voice whether or not
// anything is being recorded, so the policy is to hold VOX off for the duration
// and give it back. These cases pin that split: VOX decides the hold, never the
// refusal, and the refusal is only ever about the mic source.
//
// Run: ./build/voice_keyer_record_gate_test

#include "models/VoiceKeyerRecordGate.h"

#include <QString>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

const QString kPc  = QStringLiteral("PC");
const QString kMic = QStringLiteral("MIC");

} // namespace

int main()
{
    // The radio takes transmit audio from this computer: record.
    report("no_selectable_inputs_may_record",
           voiceKeyerRecordRefusal(false, QString()).isEmpty());

    // Selectable inputs with PC chosen: record.
    report("pc_mic_source_may_record",
           voiceKeyerRecordRefusal(true, kPc).isEmpty());

    // Selectable inputs with the hardware mic chosen: the tap would be silent.
    {
        const QString why = voiceKeyerRecordRefusal(true, kMic);
        report("hardware_mic_source_is_refused",
               why.contains(QLatin1String("MIC")) && why.contains(QLatin1String("PC")),
               why.toStdString());
    }

    // Not set is named as such rather than left blank in the message.
    {
        const QString why = voiceKeyerRecordRefusal(true, QString());
        report("unset_mic_source_is_named",
               why.contains(QLatin1String("not set")), why.toStdString());
    }

    // VOX is NOT a refusal, deliberately. Refusing the recording would not stop
    // the transmission — VOX keys on the operator's voice whether anything is
    // being recorded or not — so recording holds VOX off instead. A gate that
    // refused here would leave the hazard in place and block recording too.
    // Arguments: connected, radio has VOX, VOX on, restore in flight, VOX confirmed.
    report("vox_is_held_off_not_refused",
           voxHoldForRecording(true, true, true, false, true) == VoxHold::HoldAndRestore
               && voiceKeyerRecordRefusal(true, kPc).isEmpty()
               && voiceKeyerRecordRefusal(false, QString()).isEmpty());

    // Nothing to hold when the radio has said VOX is off.
    report("confirmed_vox_off_needs_no_hold",
           voxHoldForRecording(true, true, false, false, true) == VoxHold::None);

    // A radio that is not connected cannot be keyed, and one with no VOX that
    // AetherSDR controls (the Hermes-Lite 2) has nothing to hold, so neither is
    // sent anything, whatever the model's flags say.
    report("disconnected_or_voxless_radio_is_left_alone",
           voxHoldForRecording(false, true, true, false, true) == VoxHold::None
               && voxHoldForRecording(false, true, false, true, false) == VoxHold::None
               && voxHoldForRecording(true, false, true, true, false) == VoxHold::None);

    // The previous recording's VOX restore has not been answered yet: the model
    // still reads off, but VOX is on its way back on, so the next recording
    // holds it again rather than recording into a radio about to key (#6292).
    report("restore_in_flight_is_held_like_vox_on",
           voxHoldForRecording(true, true, false, true, true) == VoxHold::HoldAndRestore);

    // The model reads off only because an operator click asked for it; the
    // radio may answer late or refuse (#6292 re-review). Recording asks the
    // radio to confirm, and gives nothing back afterwards — off was wanted.
    report("unconfirmed_vox_off_is_confirmed_not_restored",
           voxHoldForRecording(true, true, false, false, false) == VoxHold::ConfirmOff);

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
