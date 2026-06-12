#pragma once

#include "core/tnc/Ax25.h"

#include <QString>
#include <QStringList>

#include <optional>

// Lightweight APRS (Automatic Packet Reporting System) info-field codec.
//
// Parses decoded AX.25 UI frames into a typed Packet (position reports in
// uncompressed, compressed and Mic-E form, status, messages/acks, objects,
// items, positionless weather) and builds the info-field text for the
// outbound beacon / message / ack paths. Deliberately self-contained (Qt Core
// only, no DSP, no network) so it can be unit-tested standalone, mirroring
// the ax25:: primitives one directory over. Reference: APRS Protocol
// Reference 1.0.1 (aprs.org), chapters 6-12.

namespace AetherSDR::aprs {

enum class PacketType {
    Position,   // ! = / @ data type indicators, plus all Mic-E forms
    Status,     // >
    Message,    // :ADDRESSEE :text{NN
    MessageAck, // :ADDRESSEE :ackNN
    MessageRej, // :ADDRESSEE :rejNN
    Object,     // ;
    Item,       // )
    Weather,    // _ (positionless weather report)
    Telemetry,  // T
    Query,      // ?
    Other,
};

// Decoded weather data (APRS 1.0.1 ch. 12), from a positionless '_' report
// or a position report carrying the WX symbol. Sentinel defaults mean "not
// reported" — stations send whatever sensors they have.
struct WeatherReport {
    bool valid{false};
    int windDirDeg{-1};            // < 0 unknown
    double windMph{-1.0};          // < 0 unknown
    double gustMph{-1.0};          // < 0 unknown
    double temperatureF{-1000.0};  // < -999 unknown
    int humidityPct{-1};           // < 0 unknown ("h00" means 100%)
    double pressureMbar{-1.0};     // < 0 unknown
    double rainHourIn{-1.0};       // < 0 unknown (inches)
    double rain24hIn{-1.0};
    double rainMidnightIn{-1.0};
};

// "77°F   Wind 220° 5 mph (gust 12)   Hum 50%   29.23 inHg   Rain 0.12\"/hr"
// — triple-spaced groups so it reads cleanly in the station table.
QString weatherSummary(const WeatherReport& wx);

struct Packet {
    QString source;      // "N0CALL-9"
    QString destination; // raw AX.25 destination field (tocall / Mic-E lat)
    QStringList path;    // digipeater path; repeated hops carry a trailing '*'
    PacketType type{PacketType::Other};

    // Position data — valid only when hasPosition is true.
    bool hasPosition{false};
    double latitude{0.0};   // decimal degrees, +N
    double longitude{0.0};  // decimal degrees, +E
    char symbolTable{'/'};  // '/', '\' or overlay character
    char symbolCode{'-'};
    double courseDeg{-1.0};   // < 0 when not reported
    double speedKnots{-1.0};  // < 0 when not reported
    bool hasAltitude{false};
    double altitudeFeet{0.0};
    bool messagingCapable{false}; // '=' / '@' senders accept APRS messages

    // Free-text payload: the position comment, status text, or weather data.
    QString comment;

    // Message fields (type Message / MessageAck / MessageRej).
    QString addressee;   // normalized: trimmed, upper-cased
    QString messageText;
    QString messageNo;   // empty when the sender did not request an ack

    // Object / item name (type Object / Item).
    QString objectName;

    // Weather data (type Weather). When valid, `comment` already holds the
    // human-readable weatherSummary() text.
    WeatherReport weather;

    QString infoText;    // the whole raw info field, for logging/raw view
};

// Parse a decoded AX.25 frame into an APRS packet. Returns nullopt when the
// frame is not a UI frame or has an empty info field. Frames that are UI but
// not recognizably APRS still return a Packet with type Other so the caller
// can count/log the station.
std::optional<Packet> parseFrame(const ax25::Frame& frame);

// --- Info-field encoders (return Latin-1-safe text) ---------------------

// "=4903.50N/07201.75W-comment" (or '!' when not messaging-capable).
QString encodeUncompressedPosition(double lat, double lon,
                                   char symbolTable, char symbolCode,
                                   const QString& comment,
                                   bool messagingCapable = true);

// ":ADDRESSEE:text{NN" — msgNo may be empty for fire-and-forget messages.
QString encodeMessage(const QString& addressee, const QString& text,
                      const QString& msgNo);

// ":ADDRESSEE:ackNN"
QString encodeAck(const QString& addressee, const QString& msgNo);

// ">status text"
QString encodeStatus(const QString& text);

// --- Geo helpers ---------------------------------------------------------

// 6-character Maidenhead locator ("DN18rg").
QString gridSquare(double lat, double lon);

// Great-circle distance (statute miles) and initial bearing (0-360 deg).
double distanceMiles(double lat1, double lon1, double lat2, double lon2);
double bearingDeg(double lat1, double lon1, double lat2, double lon2);

// Human-readable name for the common APRS symbols ("Home", "Car", ...);
// falls back to "Symbol 'X'" for codes not in the table.
QString symbolDescription(char symbolTable, char symbolCode);

QString packetTypeName(PacketType type);

} // namespace AetherSDR::aprs
