#pragma once

#include <QMetaType>
#include <QSet>
#include <QString>

#include <optional>

namespace AetherSDR {

// One band's TX settings as the radio reported them: the transmit plane
// (power, inhibit, hardware ALC) and the interlock plane (TX request inputs
// and the outputs keyed on that band). Present-only.
struct TxBandDelta {
    int bandId{-1};
    std::optional<QString> bandName;
    // Transmit plane ("transmit band N …").
    std::optional<int> rfPower;
    std::optional<int> tunePower;
    std::optional<bool> inhibit;
    std::optional<bool> hwAlc;
    // Interlock plane ("interlock band N …").
    std::optional<bool> accTxReq;
    std::optional<bool> rcaTxReq;
    std::optional<bool> accTx;
    std::optional<bool> tx1;
    std::optional<bool> tx2;
    std::optional<bool> tx3;
    // The output fields this report carried (acc_tx_enabled, tx1_enabled, …),
    // so TUNE inhibit knows which outputs the radio has told us about.
    QSet<QString> reportedOutputs;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::TxBandDelta)
