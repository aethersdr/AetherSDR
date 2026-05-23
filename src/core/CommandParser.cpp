#include "CommandParser.h"
#include <QStringList>

namespace MasterSDR {

// ─── Static helpers ──────────────────────────────────────────────────────────

QMap<QString, QString> CommandParser::parseKVs(const QString& body)
{
    QMap<QString, QString> result;
    // Body may look like: "freq=14.225000 mode=USB filter_lo=-1500 filter_hi=1500"
    for (const QString& token : body.split(' ', Qt::SkipEmptyParts)) {
        const int eq = token.indexOf('=');
        if (eq < 0) {
            // bare word — store with empty value so callers can detect presence
            result.insert(token, QString{});
        } else {
            result.insert(token.left(eq), token.mid(eq + 1));
        }
    }
    return result;
}

// ─── parseLine ───────────────────────────────────────────────────────────────
//
// Protocol summary (all lines are ASCII, terminated with \n):
//   V3.3.28.0                         → version
//   H0A1B2C3D                         → hex client handle
//   R1|0|                             → response to command seq 1, code 0 (OK)
//   R2|50001001|No Such Object        → error response
//   S0A1B2C3D|slice 0 freq=14.225000  → status update for slice
//   M0A1B2C3D|0053006F006D0065...     → encoded message (hex UTF-16)

ParsedMessage CommandParser::parseLine(const QString& rawLine)
{
    ParsedMessage msg;
    msg.raw = rawLine.trimmed();

    if (msg.raw.isEmpty()) return msg;

    const QChar tag = msg.raw[0];
    const QString body = msg.raw.mid(1);  // everything after the tag character

    switch (tag.toLatin1()) {
    case 'V':
        msg.type   = MessageType::Version;
        msg.object = body;  // version string
        break;

    case 'H':
        msg.type   = MessageType::Handle;
        msg.handle = body.toUInt(nullptr, 16);
        break;

    case 'R': {
        // R<seq>|<code>|<message_body>
        msg.type = MessageType::Response;
        const QStringList parts = body.split('|');
        if (parts.size() >= 1) msg.sequence   = parts[0].toUInt();
        if (parts.size() >= 2) msg.resultCode = parts[1].toInt(nullptr, 16);
        if (parts.size() >= 3) {
            msg.object = parts[2];
            msg.kvs    = parseKVs(parts[2]);
        }
        break;
    }

    case 'S': {
        // S<handle>|<object_name> [key=val ...]
        //
        // Object names can be multi-word, e.g. "slice 0", "display pan 0x40000000".
        // KV tokens always contain '='; object tokens never do.
        // Find the boundary by locating the last space before the first '='.
        msg.type = MessageType::Status;
        const int pipe = body.indexOf('|');
        if (pipe < 0) break;
        msg.handle = body.left(pipe).toUInt(nullptr, 16);
        const QString statusBody = body.mid(pipe + 1);

        const int firstEq = statusBody.indexOf('=');
        if (firstEq < 0) {
            // No KVs — entire body is the object name
            msg.object = statusBody.trimmed();
        } else {
            const int kvStart = statusBody.lastIndexOf(' ', firstEq);
            if (kvStart < 0) {
                // Body starts directly with a KV (no object prefix)
                msg.kvs = parseKVs(statusBody);
            } else {
                msg.object = statusBody.left(kvStart).trimmed();
                msg.kvs    = parseKVs(statusBody.mid(kvStart + 1));
            }
        }
        break;
    }

    case 'M':
        msg.type = MessageType::Message;
        {
            const int pipe = body.indexOf('|');
            if (pipe >= 0) {
                // The number before the '|' is hex.  Bits 24-25 encode
                // severity (Info=0, Warning=1, Error=2, Fatal=3) per
                // FlexLib Radio.cs:4498-4516; the remaining bits are an
                // opaque message id.  Reuse the same field as 'handle' to
                // keep the struct stable — readers that want the severity
                // use msg.severity, readers that need the raw id still
                // get it from msg.handle.
                msg.handle = body.left(pipe).toUInt(nullptr, 16);
                msg.severity = static_cast<MessageSeverity>((msg.handle >> 24) & 0x3);
                msg.object = body.mid(pipe + 1);
            }
        }
        break;

    default:
        msg.type = MessageType::Unknown;
        break;
    }

    return msg;
}

// ─── buildCommand ─────────────────────────────────────────────────────────────

QByteArray CommandParser::buildCommand(quint32 seq, const QString& command)
{
    return QString("C%1|%2\n").arg(seq).arg(command).toUtf8();
}

} // namespace MasterSDR
