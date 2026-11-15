#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace AetherSDR {

// A single N1MM/DXLog contest-logger spot, as broadcast by the SmartSDR-CAT
// compatible N1MMSpot UDP XML protocol (#2906). One <spot>...</spot> document
// per UDP datagram.
struct N1mmSpot {
    QString dxCall;
    double  freqMhz{0.0};      // converted from the wire's kHz value
    QString mode;
    QString spotterCall;
    QString comment;
    QString stationName;       // <StationName> — the logger PC/operator name
    QString statusFlag;        // normalized single flag for coloring: "bust",
                                // "dupe", "mult", "cq", "busy", "qtc", "new", or ""
    QString statusRaw;         // raw <statuslist> (or <status>) text, e.g. "single mult"
    QDateTime timestamp;       // UTC, from <timestamp> ("yyyy-MM-dd HH:mm:ss")
};

} // namespace AetherSDR

// Pure, dependency-light parsing for the N1MMSpot XML protocol — split out
// from N1MMSpotClient (the QUdpSocket listener) so it can be unit tested
// without pulling in logging/socket infrastructure (mirrors SpotModeResolver).
namespace AetherSDR::N1MMSpotParser {

// The N1MM status flags AetherSDR renders, in normalizeStatusFlag() priority
// order. Single source of truth for both the spot-colour lookup and the
// SpotHub colour-picker UI, so the two cannot drift apart.
//
// Defaults are theme TOKENS, not literals: an operator who hasn't picked a
// colour follows the active theme, and the contest palette stays consistent
// with the rest of the UI. settingsKey holds a per-flag override when set.
struct StatusColorSpec {
    const char* flag;         // normalized flag; "" = spot carries no status
    const char* label;        // UI label in the SpotHub N1MM tab
    const char* settingsKey;  // AppSettings key for the operator's override
    const char* themeToken;   // fallback token when no override is stored
};

inline constexpr StatusColorSpec kStatusColorSpecs[] = {
    {"bust", "Busted call",       "N1MMStatusColorBust", "color.accent.danger"},
    {"dupe", "Dupe",              "N1MMStatusColorDupe", "color.text.label"},
    {"mult", "Needed multiplier", "N1MMStatusColorMult", "color.accent"},
    {"cq",   "CQ frequency",      "N1MMStatusColorCq",   "color.accent.success"},
    {"busy", "Busy (marked)",     "N1MMStatusColorBusy", "color.accent.warning"},
    {"qtc",  "QTC",               "N1MMStatusColorQtc",  "color.accent.bright"},
    {"new",  "New QSO (not mult)","N1MMStatusColorNew",  "color.text.primary"},
    {"",     "No status",         "N1MMStatusColorNone", "color.text.secondary"},
};

// Parses one <spot>...</spot> XML document. Returns false if the datagram
// isn't well-formed, or dxcall/frequency are missing or invalid. On success,
// outAction is "add" or "delete" (defaults to "add" if <action> is absent).
bool parsePacket(const QByteArray& data, N1mmSpot& outSpot, QString& outAction);

// Name of the document's root element, or empty if the datagram isn't XML.
// N1MM sends several document types to the same destinations (RadioInfo,
// contactinfo, ...), so a spot listener needs to tell "not a spot broadcast"
// apart from "malformed spot".
QString documentRoot(const QByteArray& data);

// Stable per-spot identity: same callsign on the same band replaces the
// existing spot; same callsign on a different band is a new spot (#2906).
QString spotKey(const QString& dxCall, double freqMhz);

// Reduces a space-separated <statuslist>/<status> token string (e.g.
// "single mult") to the single most salient flag for coloring, by priority:
// bust > dupe > mult > cq > busy > qtc > new > "".
QString normalizeStatusFlag(const QString& statusList);

} // namespace AetherSDR::N1MMSpotParser
