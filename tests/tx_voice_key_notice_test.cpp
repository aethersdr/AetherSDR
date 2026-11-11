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

    // Capability shapes, as the backends declare them. Only a radio this
    // computer modulates and may key gets either follow-up (#7168 review).
    RadioCapabilities hl2;
    hl2.canTransmit = true;
    hl2.takesTxAudioOverSeam = true;
    hl2.hostModulates = true;
    RadioCapabilities icom;
    icom.canTransmit = true;
    icom.takesTxAudioOverSeam = true;
    RadioCapabilities flex;
    flex.canTransmit = true;
    flex.hasSelectableMicInputs = true;
    RadioCapabilities receiveOnlyHost = hl2;
    receiveOnlyHost.canTransmit = false;

    // Once per stretch of the missing-input state.
    NoInputVoiceKeyNotice notice;
    check("first voice key without an input device raises the notice",
          notice.takeVoiceKey(hl2, TxAudioPathBlock::NoInputDevice), true);
    check("second voice key in the same state stays quiet",
          notice.takeVoiceKey(hl2, TxAudioPathBlock::NoInputDevice), false);
    notice.observe(TxAudioPathBlock::NoInputDevice);
    check("a refresh in the same state does not re-arm it",
          notice.takeVoiceKey(hl2, TxAudioPathBlock::NoInputDevice), false);
    notice.observe(TxAudioPathBlock::None);
    check("the state clearing re-arms it",
          notice.takeVoiceKey(hl2, TxAudioPathBlock::NoInputDevice), true);
    check("a key with the device present raises nothing",
          notice.takeVoiceKey(hl2, TxAudioPathBlock::None), false);
    check("a key after the device was seen again raises it once more",
          notice.takeVoiceKey(hl2, TxAudioPathBlock::NoInputDevice), true);
    NoInputVoiceKeyNotice other;
    for (TxAudioPathBlock block : {TxAudioPathBlock::None, TxAudioPathBlock::PcAudio,
                                   TxAudioPathBlock::MicInput}) {
        check("other route blocks never raise this notice",
              other.takeVoiceKey(hl2, block), false);
    }
    check("PC Audio off in between re-arms it",
          other.takeVoiceKey(hl2, TxAudioPathBlock::NoInputDevice), true);

    // A networked Icom with PC Audio on and no device classifies as
    // NoInputDevice (#7147), but its voice key raises no notice.
    const TxAudioPathBlock icomBlock = classifyTxAudioPath(true, icom, true, u"MIC", true);
    check("Icom without an input device still classifies as NoInputDevice",
          icomBlock == TxAudioPathBlock::NoInputDevice, true);
    NoInputVoiceKeyNotice icomNotice;
    check("Icom voice key without an input device raises no notice",
          icomNotice.takeVoiceKey(icom, icomBlock), false);
    check("a second Icom voice key raises none either",
          icomNotice.takeVoiceKey(icom, icomBlock), false);
    NoInputVoiceKeyNotice flexNotice;
    check("Flex voice key without an input device raises no notice",
          flexNotice.takeVoiceKey(flex, classifyTxAudioPath(true, flex, true, u"PC", true)),
          false);
    check("Flex raises no notice even if the block were NoInputDevice",
          flexNotice.takeVoiceKey(flex, TxAudioPathBlock::NoInputDevice), false);
    check("a receive-only host-modulated backend raises no notice",
          NoInputVoiceKeyNotice{}.takeVoiceKey(receiveOnlyHost,
                                               TxAudioPathBlock::NoInputDevice),
          false);
    NoInputVoiceKeyNotice afterIcom;
    (void)afterIcom.takeVoiceKey(icom, TxAudioPathBlock::NoInputDevice);
    check("an Icom key does not use up the HL2 notice",
          afterIcom.takeVoiceKey(hl2, TxAudioPathBlock::NoInputDevice), true);

    // The chain's MIC endpoint on the HL2.
    check("HL2 PC mic with an input device is ready",
          pcMicChainReady(hl2, u"PC", false, false), true);
    check("HL2 PC mic without an input device is not ready",
          pcMicChainReady(hl2, u"PC", false, true), false);
    check("HL2 DAX TX on is not ready", pcMicChainReady(hl2, u"PC", true, false), false);
    check("HL2 radio mic is not ready", pcMicChainReady(hl2, u"MIC", false, false), false);

    // Flex, Icom and a receive-only host-modulated backend keep today's rule,
    // micSelection == "PC" && !daxOn, whatever the device state.
    for (const RadioCapabilities* caps : {&flex, &icom, &receiveOnlyHost}) {
        for (QStringView mic : {QStringView(u"PC"), QStringView(u"MIC"),
                                QStringView(u"BAL"), QStringView(u"")}) {
            for (int bits = 0; bits < 4; ++bits) {
                const bool daxOn = bits & 1;
                const bool missing = bits & 2;
                check("non-host-modulated MIC readiness is today's rule",
                      pcMicChainReady(*caps, mic, daxOn, missing),
                      mic == u"PC" && !daxOn);
            }
        }
    }
    check("Flex PC mic without an input device stays ready",
          pcMicChainReady(flex, u"PC", false, true), true);
    check("Icom PC mic without an input device stays ready",
          pcMicChainReady(icom, u"PC", false, true), true);
    return failures == 0 ? 0 : 1;
}
