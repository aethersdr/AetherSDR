#include "gui/RfGainPresentation.h"

#include <cstdio>

using namespace AetherSDR;

int main()
{
    bool ok = true;
    const auto check = [&ok](bool condition, const char* label) {
        std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
        ok = ok && condition;
    };

    check(formatRfGainIndicator(16, QStringLiteral(" dB"))
              == QStringLiteral("+16 dB"),
          "Flex dB gain keeps its signed dB presentation");
    check(formatRfGainIndicator(-8, QStringLiteral("dB"))
              == QStringLiteral("-8 dB"),
          "negative Flex gain is not double-signed");
    check(formatRfGainIndicator(100, QStringLiteral("%"))
              == QStringLiteral("RFG 100%"),
          "Icom opaque RF gain is labelled as percent, not +100 dB");
    const QStringList preampLabels{
        QStringLiteral("OFF"), QStringLiteral("P.AMP INT")};
    check(formatPreampIndicator(preampLabels, 0).isEmpty(),
          "an off preamp adds no panadapter indicator");
    check(formatPreampIndicator(preampLabels, 1)
              == QStringLiteral("P.AMP INT"),
          "an active preamp shows the radio-adopted internal state");

    return ok ? 0 : 1;
}
