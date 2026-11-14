#include "core/MicrohamTrioWebSocket.h"

#include "core/MicrohamTrioProtocol.h"

#include <QCryptographicHash>
#include <QList>

namespace AetherSDR {
namespace MicrohamTrio {
namespace Ws {

namespace {

const QByteArray kHeadEnd  = QByteArrayLiteral("\r\n\r\n");
const QByteArray kLineEnd  = QByteArrayLiteral("\r\n");
const QByteArray kWsGuid   = QByteArrayLiteral("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
const QByteArray kWidStart = QByteArrayLiteral("WID=");

inline constexpr int kMaxHostChars     = 253;
inline constexpr int kMaxWidChars      = 64;
inline constexpr int kMaxControlBytes  = 125;
inline constexpr quint8 kFinBit        = 0x80;
inline constexpr quint8 kReservedBits  = 0x70;
inline constexpr quint8 kMaskBit       = 0x80;

// A host name or address literal, safe to write after "Host: ".
bool isHeaderSafeHost(const QString& host)
{
    if (host.isEmpty() || host.size() > kMaxHostChars) {
        return false;
    }
    for (const QChar c : host) {
        const char16_t u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                        || (u >= '0' && u <= '9') || u == '.' || u == '-' || u == ':'
                        || u == '[' || u == ']';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// The observed value is a decimal integer; anything a cookie value may hold
// without quoting is taken.
bool isHeaderSafeWid(const QByteArray& wid)
{
    if (wid.isEmpty() || wid.size() > kMaxWidChars) {
        return false;
    }
    for (const char c : wid) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool hasToken(const QByteArray& headerValue, const QByteArray& token)
{
    const QList<QByteArray> parts = headerValue.toLower().split(',');
    for (const QByteArray& part : parts) {
        if (part.trimmed() == token) {
            return true;
        }
    }
    return false;
}

} // namespace

QByteArray HttpHead::value(const QByteArray& name) const
{
    for (const HttpHeader& header : headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

HttpHead parseHead(const QByteArray& bytes)
{
    HttpHead out;
    const qsizetype end = bytes.indexOf(kHeadEnd);
    if (end < 0) {
        out.complete = bytes.size() > kMaxHeadBytes;
        return out;
    }
    out.complete = true;
    if (end > kMaxHeadBytes) {
        return out;
    }
    out.bodyOffset = static_cast<int>(end + kHeadEnd.size());

    const QList<QByteArray> lines = bytes.left(end).split('\n');
    const QByteArray statusLine = lines.value(0).trimmed();
    const QList<QByteArray> words = statusLine.split(' ');
    if (!statusLine.startsWith("HTTP/") || words.size() < 2) {
        return out;
    }
    bool isNumber = false;
    const int status = words.at(1).toInt(&isNumber);
    if (!isNumber || status < 100 || status > 599) {
        return out;
    }
    out.status = status;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        out.headers.append({line.left(colon).trimmed().toLower(),
                            line.mid(colon + 1).trimmed()});
    }
    out.valid = true;
    return out;
}

std::optional<qint64> contentLength(const HttpHead& head)
{
    const QByteArray text = head.value(QByteArrayLiteral("content-length"));
    bool isNumber = false;
    const qint64 length = text.toLongLong(&isNumber);
    if (!isNumber || length < 0) {
        return std::nullopt;
    }
    return length;
}

QByteArray buildCookieRequest(const QString& host, quint16 port)
{
    if (!isHeaderSafeHost(host)) {
        return {};
    }
    // The default port is left off the Host line, as the recorded client did.
    QByteArray hostLine = host.toLatin1();
    if (port != 80) {
        hostLine += ':' + QByteArray::number(port);
    }
    return QByteArrayLiteral("GET /ui.shtml HTTP/1.1\r\nHost: ") + hostLine
           + QByteArrayLiteral("\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
}

Cookie parseCookie(const HttpHead& head)
{
    Cookie out;
    for (const HttpHeader& header : head.headers) {
        if (header.name != QByteArrayLiteral("set-cookie")) {
            continue;
        }
        const qsizetype start = header.value.indexOf(kWidStart);
        if (start < 0) {
            continue;
        }
        QByteArray wid = header.value.mid(start + kWidStart.size());
        const qsizetype semicolon = wid.indexOf(';');
        if (semicolon >= 0) {
            wid.truncate(semicolon);
        }
        wid = wid.trimmed();
        out.error = isHeaderSafeWid(wid) ? CookieError::None : CookieError::UnsafeValue;
        if (out.error == CookieError::None) {
            out.wid = wid;
        }
        return out;
    }
    return out;
}

QByteArray makeKey(const QByteArray& nonce)
{
    return nonce.toBase64();
}

QByteArray acceptFor(const QByteArray& key)
{
    return QCryptographicHash::hash(key + kWsGuid, QCryptographicHash::Sha1).toBase64();
}

QByteArray buildUpgradeRequest(const QString& host, quint16 port,
                               const QByteArray& key, const QByteArray& wid)
{
    if (!isHeaderSafeHost(host) || !isHeaderSafeWid(wid) || key.isEmpty()) {
        return {};
    }
    // Header order, and the port always on the Host line, as the recorded
    // client sent them.
    return QByteArrayLiteral("GET /ui.ws HTTP/1.1\r\nHost: ") + host.toLatin1() + ':'
           + QByteArray::number(port)
           + QByteArrayLiteral("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "Sec-WebSocket-Key: ")
           + key
           + QByteArrayLiteral("\r\nSec-WebSocket-Version: 13\r\nCookie: WID=") + wid
           + kHeadEnd;
}

Upgrade checkUpgradeResponse(const QByteArray& bytes, const QByteArray& key)
{
    Upgrade out;
    const HttpHead head = parseHead(bytes);
    if (!head.complete) {
        return out;
    }
    if (!head.valid) {
        out.result = UpgradeResult::Malformed;
        return out;
    }
    out.status = head.status;
    if (head.status >= 300 && head.status <= 399) {
        out.result = UpgradeResult::Redirected;
        return out;
    }
    if (head.status == 400) {
        out.result = UpgradeResult::CookieRefused;
        return out;
    }
    if (head.status != 101) {
        out.result = UpgradeResult::Refused;
        return out;
    }
    if (head.value(QByteArrayLiteral("upgrade")).toLower() != QByteArrayLiteral("websocket")
        || !hasToken(head.value(QByteArrayLiteral("connection")), QByteArrayLiteral("upgrade"))) {
        out.result = UpgradeResult::NotWebSocket;
        return out;
    }
    const QByteArray accept = head.value(QByteArrayLiteral("sec-websocket-accept"));
    if (accept.isEmpty()) {
        out.result = UpgradeResult::AcceptMissing;
        return out;
    }
    if (key.isEmpty() || accept != acceptFor(key)) {
        out.result = UpgradeResult::AcceptMismatch;
        return out;
    }
    out.result = UpgradeResult::Accepted;
    out.remainder = bytes.mid(head.bodyOffset);
    return out;
}

QByteArray encodeClientFrame(Opcode opcode, const QByteArray& payload, quint32 mask)
{
    QByteArray out;
    out.reserve(payload.size() + 14);
    out.append(static_cast<char>(kFinBit | static_cast<quint8>(opcode)));
    const quint64 size = static_cast<quint64>(payload.size());
    if (size < 126) {
        out.append(static_cast<char>(kMaskBit | size));
    } else if (size < 65536) {
        out.append(static_cast<char>(kMaskBit | 126));
        out.append(static_cast<char>(size >> 8));
        out.append(static_cast<char>(size & 0xff));
    } else {
        out.append(static_cast<char>(kMaskBit | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.append(static_cast<char>((size >> shift) & 0xff));
        }
    }
    const quint8 keyBytes[4] = {static_cast<quint8>(mask >> 24), static_cast<quint8>(mask >> 16),
                                static_cast<quint8>(mask >> 8), static_cast<quint8>(mask)};
    for (const quint8 keyByte : keyBytes) {
        out.append(static_cast<char>(keyByte));
    }
    for (qsizetype i = 0; i < payload.size(); ++i) {
        out.append(static_cast<char>(static_cast<quint8>(payload.at(i)) ^ keyBytes[i % 4]));
    }
    return out;
}

QVector<ServerFrame> FrameDecoder::feed(const QByteArray& bytes)
{
    QVector<ServerFrame> frames;
    if (m_error != DecodeError::None) {
        return frames;
    }
    m_buffer.append(bytes);

    const auto fail = [this, &frames](DecodeError error) {
        m_error = error;
        m_buffer.clear();
        return frames;
    };

    while (m_buffer.size() >= 2) {
        const quint8 first  = static_cast<quint8>(m_buffer.at(0));
        const quint8 second = static_cast<quint8>(m_buffer.at(1));
        const quint8 opcode = first & 0x0f;
        if ((first & kReservedBits) != 0) {
            return fail(DecodeError::ReservedBits);
        }
        if ((first & kFinBit) == 0 || opcode == static_cast<quint8>(Opcode::Continuation)) {
            return fail(DecodeError::Fragmented);
        }
        const bool control = opcode == static_cast<quint8>(Opcode::Close)
                             || opcode == static_cast<quint8>(Opcode::Ping)
                             || opcode == static_cast<quint8>(Opcode::Pong);
        const bool data = opcode == static_cast<quint8>(Opcode::Text)
                          || opcode == static_cast<quint8>(Opcode::Binary);
        if (!control && !data) {
            return fail(DecodeError::UnknownOpcode);
        }
        if ((second & kMaskBit) != 0) {
            return fail(DecodeError::Masked);
        }

        quint64 length = second & 0x7f;
        qsizetype headerSize = 2;
        if (control && length > kMaxControlBytes) {
            return fail(DecodeError::ControlTooLong);
        }
        if (length == 126 || length == 127) {
            const qsizetype extra = length == 126 ? 2 : 8;
            if (m_buffer.size() < 2 + extra) {
                break;
            }
            length = 0;
            for (qsizetype i = 0; i < extra; ++i) {
                length = (length << 8) | static_cast<quint8>(m_buffer.at(2 + i));
            }
            headerSize += extra;
        }
        if (length > static_cast<quint64>(kMaxFrameBytes)) {
            return fail(DecodeError::TooLarge);
        }
        const qsizetype total = headerSize + static_cast<qsizetype>(length);
        if (m_buffer.size() < total) {
            break;
        }
        ServerFrame frame;
        frame.firstByte = first;
        frame.payload = m_buffer.mid(headerSize, static_cast<qsizetype>(length));
        frames.append(frame);
        m_buffer.remove(0, total);
    }
    return frames;
}

void FrameDecoder::reset()
{
    m_buffer.clear();
    m_error = DecodeError::None;
}

} // namespace Ws
} // namespace MicrohamTrio
} // namespace AetherSDR
