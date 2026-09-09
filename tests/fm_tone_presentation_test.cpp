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
    check(fmToneModeDisplayLabel(QStringLiteral("ctcss_tx_dtcs_rx"))
              == QStringLiteral("CTCSS TX / DTCS RX"),
          "mixed mode label must name both tone families and directions");
    check(fmToneUsesCtcssTx(QStringLiteral("ctcss_tx_dtcs_rx")),
          "mixed TX CTCSS mode must show the TX CTCSS selector");
    check(!fmToneUsesCtcssRx(QStringLiteral("ctcss_tx_dtcs_rx")),
          "mixed TX CTCSS mode must not show the RX CTCSS selector");
    check(fmToneUsesDtcs(QStringLiteral("ctcss_tx_dtcs_rx")),
          "mixed mode must show the shared DTCS selector");
    check(fmToneUsesDtcsTx(QStringLiteral("dtcs_tx_ctcss_rx"))
              && !fmToneUsesDtcsRx(QStringLiteral("dtcs_tx_ctcss_rx"))
              && !fmToneUsesDtcsTx(QStringLiteral("ctcss_tx_dtcs_rx"))
              && fmToneUsesDtcsRx(QStringLiteral("ctcss_tx_dtcs_rx")),
          "mixed modes must classify DTCS into the correct TX or RX slot");
    check(!fmToneUsesDtcs(QStringLiteral("future_dtcs_mode")),
          "unknown capability tokens must fail closed");
    check(fmDtcsCodeRole(QStringLiteral("dtcs_tx")) == QStringLiteral("TX")
              && fmDtcsCodeRole(QStringLiteral("ctcss_tx_dtcs_rx"))
                  == QStringLiteral("RX")
              && fmDtcsCodeRole(QStringLiteral("dtcs_txrx"))
                  == QStringLiteral("TX/RX")
              && fmDtcsCodeRole(QStringLiteral("future_dtcs_mode")).isEmpty(),
          "DTCS code labels must identify their active direction and fail closed");
    const QList<FmDtcsPolarityChoice> txChoices =
        fmDtcsPolarityChoices(QStringLiteral("dtcs_tx"), false, true);
    check(txChoices.size() == 2 && txChoices[0].label == QStringLiteral("N")
              && txChoices[0].value == QStringLiteral("NR")
              && txChoices[1].label == QStringLiteral("R")
              && txChoices[1].value == QStringLiteral("RR"),
          "DTCS TX must expose one TX polarity letter and preserve RX polarity");
    const QList<FmDtcsPolarityChoice> txRxChoices =
        fmDtcsPolarityChoices(QStringLiteral("dtcs_txrx"), false, false);
    check(txRxChoices.size() == 4
              && txRxChoices[0].label == QStringLiteral("NN")
              && txRxChoices[3].label == QStringLiteral("RR"),
          "DTCS TX/RX must expose all four two-bit polarity combinations");
    const QList<FmDtcsPolarityChoice> rxChoices =
        fmDtcsPolarityChoices(QStringLiteral("ctcss_tx_dtcs_rx"), true, false);
    check(rxChoices.size() == 2 && rxChoices[0].value == QStringLiteral("RN")
              && rxChoices[1].value == QStringLiteral("RR"),
          "DTCS RX must expose one RX polarity letter and preserve TX polarity");
    check(fmDtcsPolarityChoices(QStringLiteral("future_dtcs_mode"), false, false)
              .isEmpty(),
          "unknown modes must not invent DTCS polarity choices");

    return failures == 0 ? 0 : 1;
}
