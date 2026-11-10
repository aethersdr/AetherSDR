#include "gui/TxAudioPathPolicy.h"

#include <cstdio>

using namespace AetherSDR;

// The voice-key notice for a missing audio input device (#7078): which keys
// count as voice, when the notice is raised, and the chain's MIC readiness.
int main()
{
    int failures = 0;
    const auto check = [&failures](const char* name, bool actual, bool expected) {
        const bool passed = actual == expected;
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) {
            ++failures;
        }
    };

    // Which keys the computer mic must modulate.
    for (QStringView mode : {QStringView(u"USB"), QStringView(u"LSB"),
                             QStringView(u"AM"), QStringView(u"FM"),
                             QStringView(u"DSB"), QStringView(u"usb ")}) {
        check("operator key in a voice mode is a voice key",
              isMicVoiceKey(true, false, mode), true);
    }
    check("no operator key is not a voice key", isMicVoiceKey(false, false, u"USB"), false);
    check("WSPR is not a voice key", isMicVoiceKey(true, true, u"USB"), false);
    for (QStringView mode : {QStringView(u"CW"), QStringView(u"CWL"),
                             QStringView(u"cwu"), QStringView(u"DIGU"),
                             QStringView(u"DIGL"), QStringView(u"digu")}) {
        check("CW and data modes are not voice keys", isMicVoiceKey(true, false, mode), false);
    }

    // Once per stretch of the missing-input state.
    NoInputVoiceKeyNotice notice;
    check("first voice key without an input device raises the notice",
          notice.takeVoiceKey(TxAudioPathBlock::NoInputDevice), true);
    check("second voice key in the same state stays quiet",
          notice.takeVoiceKey(TxAudioPathBlock::NoInputDevice), false);
    notice.observe(TxAudioPathBlock::NoInputDevice);
    check("a refresh in the same state does not re-arm it",
          notice.takeVoiceKey(TxAudioPathBlock::NoInputDevice), false);
    notice.observe(TxAudioPathBlock::None);
    check("the state clearing re-arms it",
          notice.takeVoiceKey(TxAudioPathBlock::NoInputDevice), true);
    check("a key with the device present raises nothing",
          notice.takeVoiceKey(TxAudioPathBlock::None), false);
    check("a key after the device was seen again raises it once more",
          notice.takeVoiceKey(TxAudioPathBlock::NoInputDevice), true);
    NoInputVoiceKeyNotice other;
    for (TxAudioPathBlock block : {TxAudioPathBlock::None, TxAudioPathBlock::PcAudio,
                                   TxAudioPathBlock::MicInput}) {
        check("other route blocks never raise this notice", other.takeVoiceKey(block), false);
    }
    check("PC Audio off in between re-arms it",
          other.takeVoiceKey(TxAudioPathBlock::NoInputDevice), true);

    // The chain's MIC endpoint.
    check("PC mic with an input device is ready", pcMicChainReady(u"PC", false, false), true);
    check("PC mic without an input device is not ready",
          pcMicChainReady(u"PC", false, true), false);
    check("DAX TX on is not ready", pcMicChainReady(u"PC", true, false), false);
    check("a radio mic is not ready", pcMicChainReady(u"MIC", false, false), false);
    return failures == 0 ? 0 : 1;
}
