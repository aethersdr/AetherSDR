#include "gui/RfGainPresentation.h"

#include <QFile>
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
    check(!shouldShowRfGainIndicator(100, 100),
          "normal fully-open Icom RF gain does not add a permanent indicator");
    check(shouldShowRfGainIndicator(0, 100),
          "Icom zero percent remains visible as meaningful attenuation");
    check(!shouldShowRfGainIndicator(0, 0),
          "neutral Flex zero dB remains hidden");
    check(normalizedRfGainUnitSuffix(QString()).compare(
              QStringLiteral("dB"), Qt::CaseInsensitive) == 0,
          "an empty suffix resets to the safe dB default");
    const QStringList preampLabels{
        QStringLiteral("OFF"), QStringLiteral("P.AMP INT")};
    check(formatPreampIndicator(preampLabels, 0).isEmpty(),
          "an off preamp adds no panadapter indicator");
    check(formatPreampIndicator(preampLabels, 1)
              == QStringLiteral("P.AMP INT"),
          "an active preamp shows the radio-adopted internal state");

    QFile wiring(QStringLiteral(AETHER_SOURCE_DIR
                                "/src/gui/MainWindow_Session.cpp"));
    check(wiring.open(QIODevice::ReadOnly),
          "the production session wiring source is available");
    const QByteArray source = wiring.readAll();
    check(source.contains("&PanadapterModel::rfGainInfoChanged")
              && source.contains("setRfGainPresentation(unitSuffix, neutral)"),
          "RF-gain unit/range metadata is wired into the spectrum widget");
    check(source.contains("&PanadapterModel::preampStepChanged")
              && source.contains("syncPreampIndicator();"),
          "radio-adopted preamp state is wired into the spectrum indicator");

    return ok ? 0 : 1;
}
