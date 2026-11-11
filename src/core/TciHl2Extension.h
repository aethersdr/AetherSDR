#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace AetherSDR {

// `aethersdr_hl2_*`, the HL2 hardware group of the extension set
// (docs/user/docs/tci-server.md). Parsing and line formatting only; TciServer
// reaches the radio through RadioModel::invokeBackendExtension("hl2", ...) and
// the health snapshot, as the Radio Setup page and the bridge do, and offers the
// group while RadioModel::backendDeclaresExtension("hl2") holds.
namespace TciHl2Extension {

inline constexpr const char* kToken = "hl2";
inline constexpr const char* kFilterBoardCommand = "aethersdr_hl2_filter_board";
inline constexpr const char* kIoBoardCommand = "aethersdr_hl2_ioboard_status";
inline constexpr const char* kTxTimingCommand = "aethersdr_hl2_tx_timing";
inline constexpr const char* kExtTrCommand = "aethersdr_hl2_exttr";

// Hl2HardwareOptions::FilterBoard: 0 none, 1 N2ADR on receive and transmit,
// 2 N2ADR on transmit only. The HPF applies only to 2, as on the setup page.
inline constexpr int kBoardTxOnly = 2;
std::optional<int> boardFromToken(const QString& token);
QString boardToken(int board);  // empty for a value outside 0..2

struct FilterBoardRequest {
    bool isSet{false};
    int board{0};
    bool hpf{false};
};
// No argument (read) or `<board>,<hpf>` (set); nullopt otherwise.
std::optional<FilterBoardRequest> parseFilterBoard(const QStringList& args);

// From a hw.get / hw.set reply; empty if it does not carry a known board.
QString filterBoardLine(const QVariantMap& hw);
// From an ioboard.status result, and the answer when there is none.
QString ioBoardLine(const QVariantMap& status);
QString ioBoardNoneLine();

// From the HL2 health snapshot's values. A field not read yet is -1.
// PTT hang 31 disables the gateware's PTT auto-unkey (softerhardware/
// Hermes-Lite2 #178), so auto_unkey is false there.
inline constexpr int kPttHangAutoUnkeyOff = 31;
QString txTimingLine(const QVariantMap& health);
// paExtTr is a transmit STATE, so a reading older than this is reported as not
// read (-1): the stream-free poller's slowest cadence is 1000 ms.
inline constexpr qint64 kExtTrFreshMs = 3000;
QString extTrLine(const QVariantMap& health);

} // namespace TciHl2Extension
} // namespace AetherSDR
