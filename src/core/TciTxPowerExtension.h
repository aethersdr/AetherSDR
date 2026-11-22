#pragma once

#include "core/backends/RadioCapabilities.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace AetherSDR {

// `aethersdr_tx_power_max`, the transmit-ceiling item of the extension set
// (docs/user/docs/tci-server.md). Read only: the radio's maximum transmit power
// at the TX frequency, from RadioCapabilities::txPowerMaxWattsAt(). Gate,
// parsing and wire text only; TciServer finds the TX slice and its frequency.
namespace TciTxPowerExtension {

inline constexpr const char* kToken = "tx_power_max";
inline constexpr const char* kCommand = "aethersdr_tx_power_max";

// The fields the ceiling is made of, copied out of a full capability record so
// a retune can be checked without rebuilding one.
RadioCapabilities limits(const RadioCapabilities& caps);

// The ceiling in watts at `frequencyHz`; 0 on a radio that cannot transmit.
double wattsAt(const RadioCapabilities& caps, double frequencyHz);

// The gate: a connected radio that can transmit, with a TX slice whose
// frequency has a ceiling above 0 W.
bool offered(bool connected, bool canTransmit, bool hasTxSlice, double watts);

// The value as sent, in tenths of a watt, so a retune that leaves it alone is
// recognised without building a line.
int tenths(double watts);

// A read: no argument, or `<trx>`. nullopt when malformed; -1 when no trx was
// named.
std::optional<int> parseRead(const QStringList& args);

// `aethersdr_tx_power_max:<trx>,<watts>;`, watts with one decimal place.
QString line(int trx, double watts);

} // namespace TciTxPowerExtension
} // namespace AetherSDR
