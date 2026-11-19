#pragma once

#include "core/backends/FrontEndOverload.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace AetherSDR {

class PanadapterModel;
struct RadioCapabilities;

// The receive gain and overload groups of the extension set
// (docs/user/docs/tci-server.md): `rx_rf_gain`, `rx_auto_rf_gain` and
// `rx_overload`. Parsing, gates and wire text only; TciServer applies a set
// through RadioModel, as the desktop does.
namespace TciRxGainExtension {

inline constexpr const char* kGainToken = "rx_rf_gain";
inline constexpr const char* kRangeCommand = "aethersdr_rx_rf_gain_range";
inline constexpr const char* kGainCommand = "aethersdr_rx_rf_gain";
inline constexpr const char* kAutoToken = "rx_auto_rf_gain";
inline constexpr const char* kAutoCommand = "aethersdr_rx_auto_rf_gain";
inline constexpr const char* kOverloadToken = "rx_overload";
inline constexpr const char* kOverloadCommand = "aethersdr_rx_overload";

struct GainRange {
    int lowDb{0};
    int highDb{0};
    int stepDb{1};
};

// The pan's range once the backend published one in dB with a positive span
// and step. A pan still holding PanadapterModel's defaults has none, and so
// does one in another unit (an Icom publishes 0-100 %).
std::optional<GainRange> panRange(const PanadapterModel& pan);

// The radio routes a pan RF gain write (RadioModel::setPanRfGainFor).
bool declaresPanRfGain(const RadioCapabilities& caps);
// rx_rf_gain: that, connected, and at least one receiver's pan with a range.
bool gainOffered(bool connected, const RadioCapabilities& caps, bool anyPanRange);
// rx_overload: a backend that observes its converter (#7176's record).
bool overloadOffered(bool connected, const RadioCapabilities& caps);

struct GainRequest {
    int trx{0};
    bool isSet{false};
    int db{0};
};
// `<trx>` (read) or `<trx>,<db>` (set), integers; nullopt otherwise.
std::optional<GainRequest> parseGain(const QStringList& args);
// Exactly `<trx>`.
std::optional<int> parseTrx(const QStringList& args);

// Clamped to the range, then to the nearest step above its low end.
int snap(int db, const GainRange& range);

QString rangeLine(int trx, const GainRange& range);
QString gainLine(int trx, int db);
QString autoLine(bool armed);
QString levelName(FrontEndLevel level);
// `<level>,<auto_armed>,<offset_db>`; the backend's free-text reason is not sent.
QString overloadLine(const FrontEndOverload& state);

} // namespace TciRxGainExtension
} // namespace AetherSDR
