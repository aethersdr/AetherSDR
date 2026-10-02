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
    return failures == 0 ? 0 : 1;
}
