#include "gui/VoiceModeGate.h"
#include <cstdio>
using namespace AetherSDR;
int main()
{
    int failures = 0;
    for (const QString& mode : {QStringLiteral("CW"), QStringLiteral("CWU"), QStringLiteral("CWL"), QStringLiteral("CWR")}) {
        if (!isCwMode(mode) || isVoiceMode(mode)) { ++failures; }
        const bool reverse = mode == QStringLiteral("CWL") || mode == QStringLiteral("CWR");
        if (isReverseCwMode(mode) != reverse) { ++failures; }
        // Zero Beat uses this shared sideband decision for the correction sign.
        const double correction = (isReverseCwMode(mode) ? -1.0 : 1.0) * (750.0 - 600.0);
        if (correction != (reverse ? -150.0 : 150.0)) { ++failures; }
    }
    for (const QString& mode : {QStringLiteral("USB"), QStringLiteral("LSB")}) {
        if (isCwMode(mode) || isReverseCwMode(mode) || !isVoiceMode(mode)) { ++failures; }
    }
    // A Flex CWL flag can remain cached when the next session uses explicit
    // modes. It must affect genuine Flex CW, but neither RTL CW nor CWR.
    if (cwReceiveSidebandSign("CW", true, true) != -1
        || cwReceiveSidebandSign("CW", true, false) != 1
        || cwReceiveSidebandSign("CW", false, true) != 1
        || cwReceiveSidebandSign("CWR", false, true) != -1
        || cwReceiveSidebandSign("CWR", false, false) != -1) { ++failures; }
    std::printf("CW/CWR mode gates: %d failures\n", failures);
    return failures ? 1 : 0;
}
