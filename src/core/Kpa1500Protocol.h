#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Elecraft KPA1500 amplifier protocol (#4097).
//
// Protocol authority: the vendor's own KPA1500 Programming Reference V3 —
// a public manufacturer document, so this is a clean-room input under
// constitution Principle IV (unlike VkampProtocol.h's reverse-engineered
// case). See docs/architecture/kpa1500-amplifier-design.md for the full
// design note, including the per-field confidence table: the COMMAND
// SPELLINGS below are quoted from that reference, but several of the
// REPLY PAYLOAD encodings (field widths and numeric scaling) still need
// confirming against real hardware before the readouts can be trusted —
// the design note's §7 checklist is what that confirmation ticks off.
//
// Framing is a single ASCII shape for every message in both directions:
//
//     '^' <CMD> [<args>] ';'
//
// <CMD> is two or three uppercase letters. A bare `^CMD;` is a QUERY; the
// same token with an argument is a SET, and the amp answers a query by
// echoing the command with its current value. That symmetry is why one
// parser handles the whole stream — there is no separate reply framing to
// decode.
//
// Transport is TCP on port 1500 (the amp also exposes a UDP server on the
// same port accepting the same command set; this implementation is TCP-only
// on purpose — see Kpa1500Connection.h).
namespace Kpa1500 {

// Default TCP control port. The amp allows this to be moved with `^CP`, so
// the Peripherals row exposes it as an editable field rather than a
// constant.
inline constexpr quint16 kDefaultPort = 1500;

// ── Wire framing ─────────────────────────────────────────────────────────

// One decoded `^CMD<arg>;` message. `arg` is the raw payload text with the
// framing stripped, NOT a parsed number — several commands answer with
// non-numeric or multi-field payloads, and keeping the raw text here means
// an unrecognized reply is still legible in a log rather than silently
// flattened to 0.
struct Message {
    QString cmd;  // e.g. "PWF", always upper-case, framing stripped
    QString arg;  // e.g. "1200"; empty for a bare query echo
};

// Builds `^CMD<arg>;`. `cmd` is upper-cased; a malformed command token
// (empty, or anything outside A-Z) yields an empty QByteArray rather than
// putting a garbage frame on the wire.
QByteArray buildMessage(const QString& cmd, const QString& arg = QString());

// Streaming frame extractor. Feed it socket bytes in whatever chunks they
// arrive in; every complete `^…;` frame fires the callback once, in order.
//
// Boundary validation per constitution Principle VII, since this consumes
// bytes straight off a LAN socket:
//   - bytes before a '^' are discarded (resynchronization), so a truncated
//     or mid-frame connect recovers instead of poisoning every later frame;
//   - a frame whose command token is not 2-3 letters A-Z is dropped, not
//     forwarded;
//   - the payload is length-capped (kMaxArgChars) so a peer that never
//     sends a ';' cannot grow the buffer without bound;
//   - the whole accumulation buffer is capped at kMaxBufferBytes.
// Nothing here throws, allocates proportionally to attacker input, or
// indexes past the end.
class MessageParser {
public:
    void setMessageCallback(std::function<void(const Message&)> cb)
    {
        m_onMessage = std::move(cb);
    }
    void feed(const QByteArray& bytes);
    void reset() { m_buf.clear(); }

    // Widest payload any documented KPA1500 reply carries, with generous
    // headroom — anything longer is a desynchronized stream, not a message.
    static constexpr int kMaxArgChars = 64;
    static constexpr int kMaxBufferBytes = 4096;

private:
    QByteArray m_buf;
    std::function<void(const Message&)> m_onMessage;
};

// Parses one complete framed message in isolation. The unit-test entry
// point; MessageParser is what the live socket path feeds. Returns nullopt
// for anything that is not exactly one well-formed frame.
std::optional<Message> parseMessage(const QByteArray& frame);

// ── Decoded amplifier state ──────────────────────────────────────────────

// ATU modes reported/commanded by `^AM`. The wire values are the enum
// values; Unknown is this layer's own "not reported yet", never sent.
enum class AtuMode { Bypass = 0, Auto = 1, Manual = 2, Unknown = -1 };

QString atuModeLabel(AtuMode mode);

// Everything the amp has told us so far. Every field is optional because
// the amp answers each query independently — a field stays unset until its
// own reply lands, so the applet can show "—" rather than a fabricated 0
// for a value the amp has not actually reported.
struct Status {
    std::optional<float> forwardWatts;    // ^PWF
    std::optional<float> reflectedWatts;  // ^PWR
    std::optional<float> swr;             // ^SW
    std::optional<int>   tempC;           // ^TM
    std::optional<int>   band;            // ^BN (raw band code, see bandName)
    std::optional<int>   faultCode;       // ^FL, 0 = no fault
    std::optional<bool>  operate;         // ^OS, true = OPERATE, false = STANDBY
    std::optional<bool>  powerOn;         // ^ON
    std::optional<int>   antenna;         // ^AN, 1-based port
    std::optional<bool>  antennaEnabled;  // ^AE
    std::optional<AtuMode> atuMode;       // ^AM
    std::optional<bool>  atuInline;       // ^AI
    std::optional<bool>  keyed;           // ^TQ — amp's own view of its key state

    // True once any field has been populated. Used to distinguish "connected
    // but nothing has answered yet" from "connected and reporting".
    bool hasAnyField() const;
};

// Folds one decoded message into `status`, returning true if it changed
// anything. An unrecognized command, or a recognized command with an
// out-of-range or non-numeric payload, leaves `status` untouched and
// returns false — malformed input is expected input here, not an error
// path (Principle VII).
bool applyMessage(const Message& message, Status& status);

// Band-code → user-facing band name for `^BN`. Returns an empty string for
// any code outside the documented table, which callers render as "—"
// rather than as a band the operator does not have.
QString bandName(int code);

// ── Command builders (host → amp) ────────────────────────────────────────
//
// Queries. The amp does not broadcast spontaneously, so the connection
// layer polls with these; see Kpa1500Connection's poll set.
QByteArray buildQuery(const QString& cmd);

// Control.
QByteArray buildSetOperate(bool operate);       // ^OS1 / ^OS0
QByteArray buildSetPower(bool on);              // ^ON1 / ^ON0
QByteArray buildSetAtuMode(AtuMode mode);       // ^AM<n>; Unknown is a no-op (empty)
QByteArray buildSetAtuInline(bool inLine);      // ^AI1 / ^AI0
QByteArray buildStartTune();                    // ^FT1
QByteArray buildCancelTune();                   // ^FE1
// `port` must be within [kMinAntenna, kMaxAntenna]; out-of-range yields an
// empty QByteArray rather than an antenna command the amp would reject.
QByteArray buildSelectAntenna(int port);        // ^AN<n>
QByteArray buildClearFault();                   // ^FL0

inline constexpr int kMinAntenna = 1;
inline constexpr int kMaxAntenna = 3;

// ── Network keying ───────────────────────────────────────────────────────
//
// `^TX;` with NO argument keys the amplifier and leaves it keyed until an
// explicit `^RX;` — with no fail-safe whatsoever if this application dies
// or the LAN drops mid-transmission. That is precisely the "radio keyed
// into an amp that cannot be told to stop" hazard #4097 asks to eliminate,
// so this layer deliberately provides NO way to build the bare form.
//
// buildKey() always emits the bounded `^TX<1..99>;` variant, whose own
// documented behaviour is that the amplifier drops out of transmit when
// the timeout expires if the controlling software stops refreshing it.
// `seconds` is clamped into [kMinKeyTimeoutSec, kMaxKeyTimeoutSec] rather
// than rejected: a caller that passes 0 must still get a SAFE frame, never
// an unbounded key and never a silently-dropped key command.
QByteArray buildKey(int seconds);
QByteArray buildUnkey();                        // ^RX
QByteArray buildKeyStateQuery();                // ^TQ

inline constexpr int kMinKeyTimeoutSec = 1;
inline constexpr int kMaxKeyTimeoutSec = 99;

}  // namespace Kpa1500
}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Kpa1500::Status)
