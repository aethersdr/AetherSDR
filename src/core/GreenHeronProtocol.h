#pragma once

// Green Heron "Everyware" antenna-switch wire protocol (TCP port 10000).
//
// PROVENANCE (Constitution Principle IV — clean-room). There is no vendor
// documentation for this protocol. Everything encoded here was determined by
// observing traffic on the wire between the vendor's own client and the
// vendor's own server (tcpdump captures, plus a hexdump probe run against a
// live installation), which Principle IV names explicitly as a clean input.
// No binary was decompiled, disassembled, or read for strings, and nothing
// here is transcribed from such output.
//
// CODE PROVENANCE, which is a separate question from the protocol provenance
// above: this is a from-scratch Qt port of the author's own prior MIT-licensed
// implementation (github.com/motoham88/everyware-linux), contributed under
// this repository's GPLv3 by the same copyright holder. Nothing is vendored.
// The two front ends that project also ships — a curses TUI and an MQTT/Home
// Assistant bridge — are deliberately NOT part of this port.
//
// What is on the far end of port 10000 is the Everyware *server* — a service
// running on a PC with the switch hardware attached to it over serial — not a
// switch. It presents several switches, and switch names are only unique
// within one server, so anything driving more than one server must key on
// host + switch name rather than switch name alone.
//
// Framing, all ASCII and line-oriented:
//
//     record   := VERB US field (US field)* CRLF
//     field    := text | subfield (GS subfield)*
//
//     US   = 0x1f   between fields
//     GS   = 0x1d   between subfields within one field
//     CRLF = 0x0d0a ends a record
//
// Record boundaries do NOT align with TCP segments. That is observed, not
// assumed: one 585-byte segment carried three whole SWITCHADD records, while
// the recurring 109-byte segment carries SWITCHLOCKS + SWITCHUPDATE +
// SWITCHLOCKS starting mid-cycle. Feed every read through splitRecords() and
// keep the remainder for next time.
//
// This translation unit is PURE — no sockets, no timers, no Qt GUI. That is
// deliberate: it lets the parser be tested against verbatim bytes captured
// off the device (tests/green_heron_protocol_test.cpp) with no hardware and
// no network. GreenHeronModel owns the socket and drives this.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace AetherSDR {
namespace GreenHeron {

// ── Wire constants ──────────────────────────────────────────────────────────

inline constexpr char kUnitSeparator  = '\x1f';
inline constexpr char kGroupSeparator = '\x1d';
inline constexpr quint16 kDefaultPort = 10000;

// The vendor's client sends a single NUL every 5.000 s — measured across 38
// consecutive keepalives (intervals 4.9987–5.0007 s). It matters: when the
// host slept, the server dropped the idle connection and the vendor client,
// unaware, retransmitted into a half-open socket for over ten minutes. That
// is why the model reconnects with backoff rather than trusting ESTABLISHED
// to mean anything.
inline constexpr int  kKeepAliveIntervalMs = 5000;
inline constexpr char kKeepAliveByte       = '\0';

// The one command the device accepts from us, confirmed byte-identical to the
// vendor client's own bytes for the same operation:
//
//     SET_SWITCH<US>AS-84F-4<US>Beam-20<CRLF>       29 bytes
//     SET_SWITCH<US>AS-84F-4<US>Dummy Load<CRLF>    32 bytes
//     SET_SWITCH<US>AS-84F-4<US>OFF<CRLF>           25 bytes
//
// Fire-and-forget: no ack, no correlation id. The device confirms by pushing
// a fresh SWITCHUPDATE ~123 ms later. Never model relay state locally from a
// command sent — the device is the authority and it reports quickly.
inline constexpr const char* kSelectVerb = "SET_SWITCH";

// ── Boundary caps (Constitution Principle VII) ──────────────────────────────
//
// Nothing arriving on this socket is trusted to be well-formed. A peer that
// never sends CRLF would otherwise grow the pending buffer without bound, and
// a record claiming thousands of fields would allocate on our side for free.
// The caps sit an order of magnitude above anything the real device emits —
// its largest observed record is the 195-byte SWITCHADD roster.

inline constexpr int kMaxRecordBytes    = 8192;
inline constexpr int kMaxPendingBytes   = 65536;
inline constexpr int kMaxFieldsPerRecord = 64;
inline constexpr int kMaxPortsPerSwitch  = 48;

// ── Records (device → client) ───────────────────────────────────────────────

enum class RecordType {
    Unknown,       // a verb we have never seen — surfaced, never fatal
    SwitchAdd,     // roster for one switch; sent for each switch on connect
    SwitchUpdate,  // current selection for one switch
    SwitchLocks,   // the interlock map, republished to every switch
};

// One selectable antenna position, as advertised in SWITCHADD.
//
// Only `name` has established meaning. Every port on every switch reported
// `0 / 0 / false`, so the captures carry no information about what the other
// three subfields do — they are carried through verbatim and never branched
// on, so an installation whose values differ is not silently reinterpreted.
struct Port {
    QString name;
    QString unknownA{QStringLiteral("0")};
    QString unknownB{QStringLiteral("0")};
    QString unknownFlag{QStringLiteral("false")};
};

struct Record {
    RecordType type{RecordType::Unknown};
    QString    verb;
    QString    switchName;

    QVector<Port> ports;        // SwitchAdd
    QString       unknownGroup; // SwitchAdd — leading field, always "1" so far

    QString selected;           // SwitchUpdate
    QString unknownC;           // SwitchUpdate — carried, never interpreted
    // The Green Heron WIRELESS LINK signal, not anything about antenna
    // selection: -27 on switches 1–2 and -28 on 3–4, stable across every
    // sample including while relays were switching. No unit is asserted — the
    // values look like they could be dBm, but that scale was never
    // established from the wire, so labelling it would be a guess presented
    // as fact.
    QString wirelessSignal;

    QStringList locks;          // SwitchLocks — slot N, in ANNOUNCEMENT order
    QStringList fields;         // Unknown — everything after the verb
};

// ── Framing ─────────────────────────────────────────────────────────────────

// Split `buffer` into complete records, leaving the partial tail in place.
//
// `bytesDropped`, when non-null, is set true if anything had to be discarded
// to stay inside the caps above — an over-long single record, or a tail that
// grew past kMaxPendingBytes without a terminator. The caller logs it; a
// well-behaved device never triggers it.
QVector<QByteArray> splitRecords(QByteArray& buffer, bool* bytesDropped = nullptr);

// Parse one complete record (no trailing CRLF). Never throws and never fails:
// an unrecognised verb or outright garbage comes back as RecordType::Unknown
// with its fields intact. A month of captures on one idle installation would
// be a thin basis for claiming the whole vocabulary, so unknown verbs are
// surfaced rather than treated as errors.
Record parse(const QByteArray& record);

// ── Commands (client → device) ──────────────────────────────────────────────

// Encode a SET_SWITCH. Returns an EMPTY array if either name contains a
// framing byte (US / GS / CR / LF): those names reach us from the device's
// own roster, which makes them untrusted input we would otherwise echo back
// as extra fields or extra records (Principle VII).
QByteArray encodeSelect(const QString& switchName, const QString& portName);

// Sort key helper: orders AS-84F-2 before AS-84F-10, and anything without a
// trailing number last. DISPLAY order only — lock slots are indexed by
// announcement order, which is a different thing entirely and on this
// hardware is 1, 3, 2, 4. See GreenHeronModel::locksBySwitch().
bool displayOrderLessThan(const QString& lhs, const QString& rhs);

} // namespace GreenHeron
} // namespace AetherSDR
