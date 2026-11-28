#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace AetherSDR {

// `aethersdr_tx_power_max`, the transmit-ceiling item of the extension set
// (docs/user/docs/tci-server.md). Read only: TransmitModel::maxPowerLevel(), the
// ceiling MQTT publishes as `max_power_level`, in whole watts. Gate, parsing
// and wire text only; TciServer reads the model and names the TX trx.
namespace TciTxPowerExtension {

inline constexpr const char* kToken = "tx_power_max";
inline constexpr const char* kCommand = "aethersdr_tx_power_max";

// The gate: a connected radio that can transmit and has reported a ceiling
// (TransmitModel::haveMaxPowerLevel()) above 0 W.
bool offered(bool connected, bool canTransmit, bool haveCeiling, int watts);

// A read: no argument, or `<trx>`. nullopt when malformed; -1 when no trx was
// named.
std::optional<int> parseRead(const QStringList& args);

// `aethersdr_tx_power_max:<trx>,<watts>;`, watts an integer (#7181: all
// values are integers).
QString line(int trx, int watts);

} // namespace TciTxPowerExtension
} // namespace AetherSDR
