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

// Parses one <spot>...</spot> XML document. Returns false if the datagram
// isn't well-formed, or dxcall/frequency are missing or invalid. On success,
// outAction is "add" or "delete" (defaults to "add" if <action> is absent).
bool parsePacket(const QByteArray& data, N1mmSpot& outSpot, QString& outAction);

// Stable per-spot identity: same callsign on the same band replaces the
// existing spot; same callsign on a different band is a new spot (#2906).
QString spotKey(const QString& dxCall, double freqMhz);

// Reduces a space-separated <statuslist>/<status> token string (e.g.
// "single mult") to the single most salient flag for coloring, by priority:
// bust > dupe > mult > cq > busy > qtc > new > "".
QString normalizeStatusFlag(const QString& statusList);

} // namespace AetherSDR::N1MMSpotParser
