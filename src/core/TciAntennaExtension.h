#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace AetherSDR {

// `aethersdr_rx_antennas` / `aethersdr_rx_antenna` and their TX pair, the
// antenna groups of the extension set (docs/user/docs/tci-server.md). Parsing
// and wire text only; TciServer applies a set through SliceModel::setRxAntenna()
// or setTxAntenna(), the path the RX applet's antenna menus use.
namespace TciAntennaExtension {

inline constexpr const char* kRxToken = "rx_antenna";
inline constexpr const char* kTxToken = "tx_antenna";
inline constexpr const char* kRxListCommand = "aethersdr_rx_antennas";
inline constexpr const char* kRxCommand = "aethersdr_rx_antenna";
inline constexpr const char* kTxListCommand = "aethersdr_tx_antennas";
inline constexpr const char* kTxCommand = "aethersdr_tx_antenna";

// The names a client can be told: the slice's list, trimmed, without empty
// names, duplicates, or a name holding a TCI reserved character (`:` `,` `;`).
QStringList wireNames(const QStringList& names);
// The listed spelling of `name`, compared without regard to case; empty when
// the list does not hold it.
QString listedName(const QStringList& wireList, const QString& name);
bool representable(const QString& name);

// The gates. RX: a connected radio with a non-empty list on some receiver.
// TX: the same, on a radio that can transmit (the RX applet offers no TX
// antenna otherwise).
bool rxOffered(bool connected, bool anyList);
bool txOffered(bool connected, bool canTransmit, bool anyList);

struct Request {
    int trx{0};
    bool isSet{false};
    QString name;
};
// `<trx>` (read) or `<trx>,<name>` (set, the selection commands only).
std::optional<Request> parse(const QStringList& args, bool allowSet);

QString listLine(const char* command, int trx, const QStringList& wireList);
QString selectionLine(const char* command, int trx, const QString& name);

} // namespace TciAntennaExtension
} // namespace AetherSDR
