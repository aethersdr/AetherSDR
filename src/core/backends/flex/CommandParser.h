#pragma once

#include <QString>
#include <QMap>
#include <QVariant>
#include <optional>

#include "core/RadioMessageTypes.h"   // MessageType / MessageSeverity (vendor-free)

namespace AetherSDR {

// Message types in the SmartSDR TCP protocol:
//   V<version>             – version announcement
//   H<handle>              – client handle assignment
//   C<seq>|<command>       – command (client → radio)
//   R<seq>|<code>|<msg>    – response (radio → client)
//   S<handle>|<status>     – status update (radio → client)
//   M<8-hex-digits>|<text> – informational/warning/error/fatal message
//                            (high 2 bits of the hex number encode severity —
//                             see MessageSeverity in core/RadioMessageTypes.h)
//
// MessageType / MessageSeverity live in core/RadioMessageTypes.h (included
// above); see there for the vendor-free-split rationale. Re-included here so
// ParsedMessage and existing includers are unchanged.

struct ParsedMessage {
    MessageType type{MessageType::Unknown};
    quint32 sequence{0};         // for R messages
    quint32 handle{0};           // for S/M messages
    int     resultCode{0};       // for R messages
    QString object;              // e.g. "slice 0"
    QString raw;                 // full raw line
    QMap<QString, QString> kvs;  // parsed key=value pairs from status/response body
    MessageSeverity severity{MessageSeverity::Info};  // for M messages only
};

// Stateless parser for SmartSDR TCP lines.
class CommandParser {
public:
    // Parse one line received from the radio.
    static ParsedMessage parseLine(const QString& line);

    // Build a command string ready to send: "C<seq>|<command>\n"
    static QByteArray buildCommand(quint32 seq, const QString& command);

    // Parse a body of key=value pairs into a map.
    static QMap<QString, QString> parseKVs(const QString& body);
};

} // namespace AetherSDR
