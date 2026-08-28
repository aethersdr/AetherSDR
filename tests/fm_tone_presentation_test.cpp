#include "gui/FmTonePresentation.h"

#include <QCoreApplication>
#include <QString>

#include <iostream>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString label = QStringLiteral("123.0");

    check(fmToneDisplayLabel(FmTonePresentation::Legacy, FmToneRole::Tx, label) == label,
          "legacy TX label must remain unchanged");
    check(fmToneDisplayLabel(FmTonePresentation::Legacy, FmToneRole::Rx, label) == label,
          "legacy RX label must remain unchanged");
    check(fmToneDisplayLabel(FmTonePresentation::Hidden, FmToneRole::Tx, label) == label,
          "hidden presentation must not invent a TX prefix");
    check(fmToneDisplayLabel(FmTonePresentation::Ctcss, FmToneRole::Tx, label)
              == QStringLiteral("TX: 123.0"),
          "CTCSS TX label must identify the transmit register");
    check(fmToneDisplayLabel(FmTonePresentation::Ctcss, FmToneRole::Rx, label)
              == QStringLiteral("RX: 123.0"),
          "CTCSS RX label must identify the receive register");
    check(legacyFmToneModes()
              == QStringList{QStringLiteral("off"), QStringLiteral("ctcss_tx")},
          "legacy mode authority must preserve established modes");

    return failures == 0 ? 0 : 1;
}
