#include "gui/TxAudioPathPolicy.h"

#include <cstdio>

using namespace AetherSDR;

int main()
{
    int failures = 0;
    const auto check = [&failures](const char* name, TxAudioPathBlock actual,
                                  TxAudioPathBlock expected) {
        const bool passed = actual == expected;
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) {
            ++failures;
        }
    };

    RadioCapabilities flex;
    flex.canTransmit = true;
    flex.hasSelectableMicInputs = true;
    check("disconnected route is unknown",
          classifyTxAudioPath(false, flex, false, u"MIC"), TxAudioPathBlock::None);
    for (QStringView source : {QStringView(u"MIC"), QStringView(u"BAL"),
                               QStringView(u"LINE"), QStringView(u"ACC"),
                               QStringView(u"")}) {
        check("Flex non-PC input bypasses client DSP",
              classifyTxAudioPath(true, flex, true, source), TxAudioPathBlock::MicInput);
    }
    check("Flex PC input remains available with PC Audio off",
          classifyTxAudioPath(true, flex, false, u"PC"), TxAudioPathBlock::None);
    check("Flex PC input remains available with PC Audio on",
          classifyTxAudioPath(true, flex, true, u"PC"), TxAudioPathBlock::None);
    check("Flex radio mic is still blocked with PC Audio off",
          classifyTxAudioPath(true, flex, false, u"MIC"), TxAudioPathBlock::MicInput);

    RadioCapabilities seam;
    seam.canTransmit = true;
    seam.takesTxAudioOverSeam = true;
    check("seam audio requires PC Audio",
          classifyTxAudioPath(true, seam, false, u"PC"), TxAudioPathBlock::PcAudio);
    check("seam audio accepts PC Audio",
          classifyTxAudioPath(true, seam, true, u"MIC"), TxAudioPathBlock::None);
    seam.hostModulates = true;
    check("host-modulated audio is not blocked",
          classifyTxAudioPath(true, seam, false, u"MIC"), TxAudioPathBlock::None);
    seam.hostModulates = false;
    seam.canTransmit = false;
    check("receive-only backend is not blocked",
          classifyTxAudioPath(true, seam, false, u"MIC"), TxAudioPathBlock::None);

    // No family names: the declared capabilities determine the policy.
    seam.canTransmit = true;
    seam.hasSelectableMicInputs = true;
    check("seam refusal takes precedence when both capabilities are present",
          classifyTxAudioPath(true, seam, false, u"MIC"), TxAudioPathBlock::PcAudio);
    check("selectable input still applies once seam audio is enabled",
          classifyTxAudioPath(true, seam, true, u"MIC"), TxAudioPathBlock::MicInput);
    RadioCapabilities other;
    other.canTransmit = true;
    check("backend without either routing restriction is available",
          classifyTxAudioPath(true, other, false, u"MIC"), TxAudioPathBlock::None);

    // No capture device on the computer (#7078). Host-modulated seam backend
    // (HL2 shape): the PC is the only TX audio source.
    RadioCapabilities hostSeam;
    hostSeam.canTransmit = true;
    hostSeam.takesTxAudioOverSeam = true;
    hostSeam.hostModulates = true;
    check("host-modulated seam audio with an input device is not blocked",
          classifyTxAudioPath(true, hostSeam, true, u"PC", false), TxAudioPathBlock::None);
    check("host-modulated seam audio without an input device is reported",
          classifyTxAudioPath(true, hostSeam, true, u"PC", true),
          TxAudioPathBlock::NoInputDevice);
    check("host-modulated missing input is reported whatever the mic selection",
          classifyTxAudioPath(true, hostSeam, true, u"MIC", true),
          TxAudioPathBlock::NoInputDevice);
    check("host-modulated missing input is silent with PC Audio off",
          classifyTxAudioPath(true, hostSeam, false, u"PC", true), TxAudioPathBlock::None);
    check("host-modulated missing input is silent while disconnected",
          classifyTxAudioPath(false, hostSeam, true, u"PC", true), TxAudioPathBlock::None);
    hostSeam.canTransmit = false;
    check("receive-only host-modulated backend is not blocked without an input",
          classifyTxAudioPath(true, hostSeam, true, u"PC", true), TxAudioPathBlock::None);
    RadioCapabilities hostNoSeam;
    hostNoSeam.canTransmit = true;
    hostNoSeam.hostModulates = true;
    check("host-modulated backend without seam audio ignores a missing input",
          classifyTxAudioPath(true, hostNoSeam, true, u"PC", true), TxAudioPathBlock::None);

    // Seam audio on a radio that modulates itself (networked Icom shape).
    RadioCapabilities radioSeam;
    radioSeam.canTransmit = true;
    radioSeam.takesTxAudioOverSeam = true;
    check("seam audio without an input device is reported",
          classifyTxAudioPath(true, radioSeam, true, u"PC", true),
          TxAudioPathBlock::NoInputDevice);
    check("PC Audio off still wins over a missing input",
          classifyTxAudioPath(true, radioSeam, false, u"PC", true),
          TxAudioPathBlock::PcAudio);
    radioSeam.hasSelectableMicInputs = true;
    check("a radio mic selection still wins over a missing input",
          classifyTxAudioPath(true, radioSeam, true, u"MIC", true),
          TxAudioPathBlock::MicInput);
    check("PC mic selection with a missing input is reported",
          classifyTxAudioPath(true, radioSeam, true, u"PC", true),
          TxAudioPathBlock::NoInputDevice);

    // Flex shape: TX audio is not taken over the seam, so a missing local
    // input never changes the outcome.
    for (QStringView source : {QStringView(u"MIC"), QStringView(u"PC")}) {
        for (bool pcOn : {false, true}) {
            const TxAudioPathBlock expected = classifyTxAudioPath(true, flex, pcOn, source);
            check("Flex outcome is unchanged by a missing input",
                  classifyTxAudioPath(true, flex, pcOn, source, true), expected);
        }
    }
    check("Flex PC input with a missing input device is not blocked",
          classifyTxAudioPath(true, flex, true, u"PC", true), TxAudioPathBlock::None);
    check("backend without routing restrictions ignores a missing input",
          classifyTxAudioPath(true, other, true, u"PC", true), TxAudioPathBlock::None);
    return failures == 0 ? 0 : 1;
}
