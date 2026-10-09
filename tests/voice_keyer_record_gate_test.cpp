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
    report("vox_is_held_off_not_refused",
           shouldHoldVoxOff(true, true)
               && voiceKeyerRecordRefusal(true, kPc).isEmpty()
               && voiceKeyerRecordRefusal(false, QString()).isEmpty());

    // Nothing to hold when VOX is off.
    report("vox_off_needs_no_hold", !shouldHoldVoxOff(true, false));

    // A radio that is not connected cannot be keyed, so a stale VOX flag is not
    // a reason to send it commands.
    report("disconnected_radio_is_left_alone", !shouldHoldVoxOff(false, true));

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
