#pragma once

#include "core/backends/RadioCapabilities.h"

#include <QList>
#include <QString>

namespace AetherSDR {

enum class FmToneRole {
    Tx,
    Rx,
};

[[nodiscard]] inline QString fmToneDisplayLabel(FmTonePresentation presentation,
                                                FmToneRole role,
                                                const QString& legacyLabel)
{
    if (presentation != FmTonePresentation::Ctcss) {
        return legacyLabel;
    }
    return role == FmToneRole::Tx
        ? QStringLiteral("TX: %1").arg(legacyLabel)
        : QStringLiteral("RX: %1").arg(legacyLabel);
}

[[nodiscard]] inline QString fmToneModeDisplayLabel(const QString& mode)
{
    if (mode == QLatin1String("off")) {
        return QStringLiteral("Off");
    }
    if (mode == QLatin1String("ctcss_tx")) {
        return QStringLiteral("CTCSS TX");
    }
    if (mode == QLatin1String("ctcss_rx")) {
        return QStringLiteral("CTCSS RX");
    }
    if (mode == QLatin1String("ctcss_txrx")) {
        return QStringLiteral("CTCSS TX/RX");
    }
    if (mode == QLatin1String("dtcs_tx")) {
        return QStringLiteral("DTCS TX");
    }
    if (mode == QLatin1String("dtcs_txrx")) {
        return QStringLiteral("DTCS TX/RX");
    }
    if (mode == QLatin1String("ctcss_tx_dtcs_rx")) {
        return QStringLiteral("CTCSS TX / DTCS RX");
    }
    if (mode == QLatin1String("dtcs_tx_ctcss_rx")) {
        return QStringLiteral("DTCS TX / CTCSS RX");
    }
    return mode.toUpper();
}

[[nodiscard]] inline bool fmToneUsesCtcssTx(const QString& mode)
{
    return mode == QLatin1String("ctcss_tx") || mode == QLatin1String("ctcss_txrx")
        || mode == QLatin1String("ctcss_tx_dtcs_rx");
}

[[nodiscard]] inline bool fmToneUsesCtcssRx(const QString& mode)
{
    return mode == QLatin1String("ctcss_rx") || mode == QLatin1String("ctcss_txrx")
        || mode == QLatin1String("dtcs_tx_ctcss_rx");
}

[[nodiscard]] inline bool fmToneUsesDtcsTx(const QString& mode)
{
    return mode == QLatin1String("dtcs_tx") || mode == QLatin1String("dtcs_txrx")
        || mode == QLatin1String("dtcs_tx_ctcss_rx");
}

[[nodiscard]] inline bool fmToneUsesDtcsRx(const QString& mode)
{
    return mode == QLatin1String("dtcs_txrx")
        || mode == QLatin1String("ctcss_tx_dtcs_rx");
}

[[nodiscard]] inline bool fmToneUsesDtcs(const QString& mode)
{
    return fmToneUsesDtcsTx(mode) || fmToneUsesDtcsRx(mode);
}

[[nodiscard]] inline QString fmDtcsCodeRole(const QString& mode)
{
    if (mode == QLatin1String("dtcs_txrx")) {
        return QStringLiteral("TX/RX");
    }
    if (fmToneUsesDtcsTx(mode) && !fmToneUsesDtcsRx(mode)) {
        return QStringLiteral("TX");
    }
    if (fmToneUsesDtcsRx(mode) && !fmToneUsesDtcsTx(mode)) {
        return QStringLiteral("RX");
    }
    return {};
}

struct FmDtcsPolarityChoice {
    QString label;
    QString value;
};

[[nodiscard]] inline QList<FmDtcsPolarityChoice> fmDtcsPolarityChoices(
    const QString& mode, bool txReverse, bool rxReverse)
{
    const QChar tx = txReverse ? QLatin1Char('R') : QLatin1Char('N');
    const QChar rx = rxReverse ? QLatin1Char('R') : QLatin1Char('N');
    if (mode == QLatin1String("dtcs_txrx")) {
        return {{QStringLiteral("NN"), QStringLiteral("NN")},
                {QStringLiteral("NR"), QStringLiteral("NR")},
                {QStringLiteral("RN"), QStringLiteral("RN")},
                {QStringLiteral("RR"), QStringLiteral("RR")}};
    }
    if (mode == QLatin1String("dtcs_tx")
        || mode == QLatin1String("dtcs_tx_ctcss_rx")) {
        return {{QStringLiteral("N"), QString(QLatin1Char('N')) + rx},
                {QStringLiteral("R"), QString(QLatin1Char('R')) + rx}};
    }
    if (mode == QLatin1String("ctcss_tx_dtcs_rx")) {
        return {{QStringLiteral("N"), QString(tx) + QLatin1Char('N')},
                {QStringLiteral("R"), QString(tx) + QLatin1Char('R')}};
    }
    return {};
}

} // namespace AetherSDR
