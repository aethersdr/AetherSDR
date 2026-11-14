#pragma once

// The HTTP and WebSocket bytes of a microHAM TRIO session: the page request
// that yields the WID cookie, the upgrade on /ui.ws, and RFC 6455 framing for
// the few frame shapes the device uses. The requests are the ones a working
// client sent to one unit; the device's own response headers were not recorded.
// Pure: no socket, no clock. Tested in tests/microham_trio_session_test.cpp.

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace AetherSDR {
namespace MicrohamTrio {
namespace Ws {

// A response head larger than this is not the device's.
inline constexpr int kMaxHeadBytes = 8192;

struct HttpHeader {
    QByteArray name;    // lower case
    QByteArray value;   // trimmed
};

struct HttpHead {
    bool complete{false};   // the blank line that ends the head was seen
    bool valid{false};      // complete, and the status line is HTTP with a code
    int  status{0};
    int  bodyOffset{0};     // first byte after the head
    QVector<HttpHeader> headers;

    // The first header of that name (lower case), or empty.
    QByteArray value(const QByteArray& name) const;
};

// Parse a response head from the start of `bytes`. Incomplete until the blank
// line arrives; a head past kMaxHeadBytes is complete and invalid.
HttpHead parseHead(const QByteArray& bytes);

// Content-Length when the head carries a usable one.
std::optional<qint64> contentLength(const HttpHead& head);

// GET /ui.shtml: the page whose response sets the WID cookie the upgrade
// needs. The cookie is a session handle, not a credential. Empty when `host`
// cannot be written into a header.
QByteArray buildCookieRequest(const QString& host, quint16 port);

enum class CookieError {
    None,
    NoCookie,      // no Set-Cookie header carries WID=
    UnsafeValue,   // the value cannot be echoed into a header
};

struct Cookie {
    CookieError error{CookieError::NoCookie};
    QByteArray  wid;
};

// The WID value from a Set-Cookie header: the text after "WID=" up to ";".
Cookie parseCookie(const HttpHead& head);

// Sec-WebSocket-Key for a 16-byte nonce, and the Sec-WebSocket-Accept a
// server must answer it with (RFC 6455 section 4.2.2).
QByteArray makeKey(const QByteArray& nonce);
QByteArray acceptFor(const QByteArray& key);

// GET /ui.ws with the upgrade headers and the cookie. Empty when `host` or
// `wid` cannot be written into a header.
QByteArray buildUpgradeRequest(const QString& host, quint16 port,
                               const QByteArray& key, const QByteArray& wid);

enum class UpgradeResult {
    Incomplete,       // the head has not ended yet
    Accepted,
    Malformed,        // not an HTTP response head: a bare page, or too large
    Redirected,       // 3xx: the device answers a plain page request this way
    CookieRefused,    // 400: the device's answer to an upgrade without a valid WID
    Refused,          // any other status than 101
    NotWebSocket,     // 101 without the Upgrade and Connection headers
    AcceptMissing,
    AcceptMismatch,
};

struct Upgrade {
    UpgradeResult result{UpgradeResult::Incomplete};
    int           status{0};
    QByteArray    remainder;   // bytes after the head: the first frames
};

// Judge the response to buildUpgradeRequest(). Only Accepted may be followed
// by frames.
Upgrade checkUpgradeResponse(const QByteArray& bytes, const QByteArray& key);

enum class Opcode : quint8 {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xa,
};

// One unfragmented client frame. A client masks every frame; `mask` is the
// four key bytes, most significant first, and must be unpredictable in use.
QByteArray encodeClientFrame(Opcode opcode, const QByteArray& payload, quint32 mask);

enum class DecodeError {
    None,
    ReservedBits,
    Fragmented,       // FIN clear, or a continuation frame
    UnknownOpcode,
    Masked,           // a server never masks
    ControlTooLong,   // a control frame over 125 bytes
    TooLarge,         // over MicrohamTrio::kMaxFrameBytes
};

struct ServerFrame {
    quint8     firstByte{0};   // FIN and opcode, as MicrohamTrio::classify() takes it
    QByteArray payload;

    Opcode opcode() const { return static_cast<Opcode>(firstByte & 0x0f); }
};

// Incremental decoder for server frames. The device sends whole text frames
// and empty pongs; anything this decoder cannot take is an error, and after an
// error it yields nothing until reset().
class FrameDecoder {
public:
    // Every frame completed by `bytes`. Frames before an error are returned.
    QVector<ServerFrame> feed(const QByteArray& bytes);
    DecodeError error() const { return m_error; }
    void reset();

private:
    QByteArray  m_buffer;
    DecodeError m_error{DecodeError::None};
};

} // namespace Ws
} // namespace MicrohamTrio
} // namespace AetherSDR
