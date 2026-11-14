#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

// The VARA-style TCP host interface that external HF data modems expose:
// an ASCII command port (CR-terminated lines) plus a data port that carries
// the session's payload as an opaque byte stream.
//
// Pure framing, parsing and formatting with no I/O, in both directions:
//   * modem -> host: notifications a modem sends (the gateway as a client)
//   * host -> modem: commands a host application sends (the gateway as the
//     server its back end connects to)
//
// Every token here is cited in docs/hf-gateway-host-interface.md, to one of
// two sources: the vendored Data2G host layer (third_party/data2g/core/host/)
// or Mercury's published docs/TNC.md. Nothing here is specific to any one
// modem implementation.
//
// Input is untrusted. Lines are length-capped and must be printable ASCII;
// tokens are matched exactly; numbers and callsigns are range-checked. A line
// that fails a check is reported as Unknown or Invalid, never acted on.

namespace AetherSDR::ModemHost {

// Longest command-port line accepted, CR excluded. Every legitimate line in
// the cited vocabulary is far shorter.
inline constexpr int kMaxLineBytes = 1024;

// A MYCALL carries a primary callsign plus at most four secondaries
// (Mercury docs/TNC.md, MYCALL).
inline constexpr int kMaxMyCallCallsigns = 5;

// BUFFER counts application bytes; anything above this is treated as a
// malformed report rather than trusted.
inline constexpr qint64 kMaxBufferBytes = 16 * 1024 * 1024;

// SN range accepted as plausible, in dB.
inline constexpr double kMinSnDb = -50.0;
inline constexpr double kMaxSnDb = 60.0;

// Default ports: command on the base port, data on base + 1
// (Mercury docs/TNC.md).
inline constexpr quint16 kDefaultCommandPort = 8300;

// One framed line. A rejected line keeps its place in the stream, because
// replies on the command port are positional: a server must still answer a
// rejected command with WRONG, in order.
struct Line {
    QString text;           // empty when rejected
    bool rejected = false;  // over kMaxLineBytes, or bytes outside printable ASCII
};

// Splits a command-port byte stream into lines. Handles reads that join
// several lines or split one. LF is ignored so a CRLF sender still frames
// cleanly; empty lines are dropped.
class LineSplitter {
public:
    QList<Line> feed(const QByteArray& bytes);
    void reset();

    // Lines rejected since construction or the last reset().
    int rejectedLines() const { return m_rejected; }

private:
    QByteArray m_line;
    bool m_overflow = false;
    bool m_badByte = false;
    int m_rejected = 0;
};

// True for a callsign the gateway will pass on: 1-15 characters of A-Z, 0-9,
// '/' and '-', starting and ending with a letter or digit. This is the union
// of Mercury's limit (15 characters) and Data2G's alphabet (A-Z 0-9 / -);
// a modem with a narrower rule answers WRONG for itself. Upper case only:
// parseCommand() upper-cases its input first.
bool isValidCallsign(const QString& call);

// A bandwidth token the cited modems use: 500, 1200, 2300 or 2750 Hz.
bool isKnownBandwidth(int hz);

// ── Modem -> host notifications ─────────────────────────────────────────────

enum class NotificationType {
    Unknown,        // not in the cited vocabulary, or failed validation
    Ok,             // positional reply to the oldest outstanding command
    Wrong,          // positional rejection of the oldest outstanding command
    IAmAlive,       // keepalive
    Pending,        // an incoming connect is being answered
    CancelPending,  // ...and did not complete
    Connected,      // source destination bandwidth
    Disconnected,   // session over
    Ptt,            // the modem keys / releases the transmitter
    Busy,           // busy detector
    Buffer,         // application bytes still unacknowledged
    SignalToNoise,  // dB
    Bitrate,        // (code) bps
    Registered,     // one callsign per line, after MYCALL's OK
    Version,        // free text
    CqFrame,        // source bandwidth
};

struct Notification {
    NotificationType type = NotificationType::Unknown;
    QString raw;

    bool on = false;          // Ptt, Busy
    QString source;           // Connected (the initiator), CqFrame
    QString destination;      // Connected (the station called)
    int bandwidthHz = 0;      // Connected, CqFrame
    qint64 bytes = 0;         // Buffer
    double snDb = 0.0;        // SignalToNoise
    int bitrateCode = 0;      // Bitrate
    int bitsPerSecond = 0;    // Bitrate
    QString callsign;         // Registered
    QString text;             // Version
};

// Decode one framed line from a modem. Never throws; anything unrecognised or
// invalid comes back as Unknown with `raw` set.
Notification parseNotification(const QString& line);

// Commands the gateway sends to a modem. Each returns the bytes to write, CR
// included. A builder given invalid input returns an empty array, which the
// caller must not send.
QByteArray cmdMyCall(const QStringList& callsigns);
QByteArray cmdListen(bool on);
QByteArray cmdDisconnect();
QByteArray cmdAbort();
QByteArray cmdVersion();
QByteArray cmdBuffer();
QByteArray cmdSn();

// ── Host -> modem commands (the gateway as server) ──────────────────────────

enum class CommandType {
    Unknown,        // not in the cited vocabulary: answered WRONG
    Invalid,        // a known command with bad arguments: answered WRONG
    MyCall,
    ListenOn,
    ListenOff,
    ListenCq,
    Connect,
    Disconnect,
    Abort,
    Version,
    Buffer,
    Sn,
    Bitrate,
    Bandwidth,      // BW500 / BW1200 / BW2300 / BW2750
    CqFrame,
    Tune,
    Encryption,
    Chat,
    Public,
    Compression,
    CleanTxBuffer,
};

struct Command {
    CommandType type = CommandType::Unknown;
    QString raw;

    QStringList callsigns;  // MyCall
    QString source;         // Connect, CqFrame
    QString destination;    // Connect
    int bandwidthHz = 0;    // Bandwidth, CqFrame
    QStringList args;       // remaining tokens, for logging
};

// Keywords and arguments are matched case-insensitively, as the Data2G host
// layer does (it upper-cases the command word and its first argument); the
// whole line is upper-cased first, so callsigns come back upper case.
Command parseCommand(const QString& line);

// Notifications the gateway sends to its back end. Each returns the bytes to
// write, CR included, or an empty array for invalid input.
QByteArray fmtOk();
QByteArray fmtWrong();
QByteArray fmtIAmAlive();
QByteArray fmtPending();
QByteArray fmtCancelPending();
QByteArray fmtConnected(const QString& source, const QString& destination, int bandwidthHz);
QByteArray fmtDisconnected();
QByteArray fmtPtt(bool on);
QByteArray fmtBusy(bool on);
QByteArray fmtBuffer(qint64 bytes);
QByteArray fmtSn(double snDb);
QByteArray fmtVersion(const QString& text);

QString toString(NotificationType type);
QString toString(CommandType type);

}  // namespace AetherSDR::ModemHost
