#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace AetherSDR {

struct RadioCapabilities;

// `aethersdr_tx_filter_band`, the TX passband group of the extension set
// (docs/user/docs/tci-server.md). Parsing and normalisation only; TciServer
// applies the result through TransmitModel::setTxFilter(), as the desktop does.
namespace TciTxPassbandExtension {

inline constexpr const char* kToken = "tx_filter_band";
inline constexpr const char* kCommand = "aethersdr_tx_filter_band";

// The gate: a connected radio that declares the TX cutoff controls.
bool offered(bool connected, const RadioCapabilities& caps);

struct Request {
    int trx{0};
    bool isSet{false};
    int lowHz{0};
    int highHz{0};
};
// `<trx>` (read) or `<trx>,<low>,<high>` (set), integers; nullopt otherwise.
std::optional<Request> parse(const QStringList& args);

// Bounded to +/-TransmitModel::kTxFilterMaxHz before abs (no INT_MIN UB),
// swapped if reversed, then snapped to the radio's edge lists when it has them.
// TransmitModel::setTxFilter() applies the range and minimum width after this.
struct Passband {
    int lowHz{0};
    int highHz{0};
};
Passband normalise(int lowHz, int highHz, const RadioCapabilities& caps);

QString stateLine(int trx, int lowHz, int highHz);

} // namespace TciTxPassbandExtension
} // namespace AetherSDR
