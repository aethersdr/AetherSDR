#pragma once

#include <QString>
#include <QMap>
#include <QVariant>
#include <optional>

namespace AetherSDR {

// Message types in the SmartSDR TCP protocol:
//   V<version>             – version announcement
//   H<handle>              – client handle assignment
//   C<seq>|<command>       – command (client → radio)
//   R<seq>|<code>|<msg>    – response (radio → client)
//   S<handle>|<status>     – status update (radio → client)
//   M<8-hex-digits>|<text> – informational/warning/error/fatal message
//                            (high 2 bits of the hex number encode severity —
//                             see MessageSeverity below; per FlexLib
//                             Radio.cs:4498-4516)

enum class MessageType {
    Version,
    Handle,
    Response,
    Status,
    Message,
    Unknown
};

// M-prefix severity, extracted from bits 24-25 of the message number per
// FlexLib's `(num >> 24) & 0x3`.  Info messages (e.g. "Client connected
// from IP …") are expected to be logged silently; Warning and above
// surface to the user.
enum class MessageSeverity {
    Info    = 0,
    Warning = 1,
    Error   = 2,
    Fatal   = 3
};

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
