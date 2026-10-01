#pragma once

// Green Heron "Everyware" antenna-switch wire protocol (TCP port 10000).
//
// Clean-room (Principle IV): there is no vendor documentation; everything here
// comes from observing traffic between the vendor's own client and server.
// Nothing was decompiled or disassembled. The code is a from-scratch Qt port of
// the author's own MIT-licensed implementation (github.com/motoham88/
// everyware-linux), contributed under GPLv3 by the same copyright holder.
//
// The far end is the Everyware server (a PC with the switch hardware on serial),
// not a switch. One socket carries both the antenna switches (SWITCHADD /
// SWITCHUPDATE / SWITCHLOCKS / SET_SWITCH) and any rotator (ADD / POINT / TURN).
// Switch names are unique only within one server, so key on host + switch name.
//
// Framing, ASCII and line-oriented:
//
//     record   := VERB US field (US field)* CRLF
//     field    := text | subfield (GS subfield)*
//
//     US = 0x1f between fields, GS = 0x1d between subfields, CRLF ends a record.
//     TURN is the exception and ends with a bare CR (see kTurnVerb).
//
// Record boundaries do not align with TCP segments (observed both ways), so feed
// every read through splitRecords() and keep the remainder.
//
// Pure — no sockets, timers or GUI — so the parser is tested against captured
// bytes (tests/green_heron_protocol_test.cpp). GreenHeronModel owns the socket.

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

// The rotator command, and the only other verb this client ever sends.
// Confirmed byte-for-byte off the wire, then confirmed accepted by moving a
// real RT-21:
//
//     54 55 52 4e 1f 52 6f 74 6f 72 1f 38 39 2e 30 0d
//     T  U  R  N  US R  o  t  o  r  US 8  9  .  0  CR
//
// THE TERMINATOR IS A BARE CR (0x0d), NOT CRLF. That is a genuine asymmetry
// with SET_SWITCH and it is why encodeTurn() does not share a terminator with
// encodeSelect(). Do not "tidy" the two into one without a capture that says
// the device accepts CRLF here.
//
// Fire-and-forget, like SET_SWITCH: no ack, no correlation id. Confirmation
// arrives as a fresh POINT. There is NO stop, park, or disable verb anywhere
// in this protocol — powering the controller off produced no record at all,
// because it is a physical action. A turn that starts cannot be recalled in
// software, which is why every front end must make choosing a heading and
// sending it two separate gestures.
inline constexpr const char* kTurnVerb = "TURN";

// Headings, in both directions, carry one decimal place. Encoding must be
// LOCALE-INDEPENDENT: a comma-decimal locale would put "89,0" on a wire that
// only ever carries "89.0".
inline constexpr int kHeadingDecimals = 1;

// TURN is refused outside this range. Deliberately strict: overlap-capable
// rotators may well accept headings past 360, but none was ever observed
// doing so, and a guess here aims a real antenna.
inline constexpr double kMinHeadingDegrees = 0.0;
inline constexpr double kMaxHeadingDegrees = 360.0;

// How long POINT may go quiet before a rotator must be treated as gone.
//
// Presence is dynamic and SILENCE IS THE ONLY SIGNAL. Powering the rotator's
// controller off is not a socket event: ADD and POINT simply stop while
// SWITCHUPDATE and SWITCHLOCKS carry on down the same connection, so nothing
// else on the wire ever contradicts the last heading. 10 s is about ten
// missed POINTs at the measured 0.97 s cadence.
inline constexpr int kRotorSilentAfterMs = 10000;

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
    DeviceAdd,     // a NAMED DEVICE announcing itself — see below, not "RotorAdd"
    Point,         // a rotator's reported heading
};

// DeviceAdd is deliberately NOT called RotorAdd. Every capture of this verb
// reads `ADD<US>Rotor`, and that field is the device's OPERATOR-CONFIGURED
// name — the same way switch records key on the switch name. That the name
// happened to be the word "Rotor" is not evidence the verb is rotor-specific:
// one installation, one device, one generic three-character verb on a socket
// shared with the switches. A rotator named "Beam" would announce
// `ADD<US>Beam`, and a future non-rotor device would land in the same branch.
//
// POINT is what establishes that a named device is a rotator with a heading.
// The model therefore keys its rotator table on POINT and treats ADD as
// corroboration, never the other way round.

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

    // DeviceAdd / Point — the operator-configured device name, the same way
    // switch records key on the switch name. Held apart from switchName
    // because the two namespaces are not the same one.
    QString deviceName;

    // Point — the REPORTED heading in degrees. Only ever this; see
    // GreenHeronModel::turnTo() for why the commanded one is never stored.
    //
    // The device reports tenths it does not resolve: 120 consecutive POINTs
    // from a stationary rotator landed on a ~0.47° grid spanning 3.7°. Treat
    // this as half-degree data wearing a tenths costume.
    double heading{0.0};

    // Point fields 3 and 4, carried verbatim and branched on by NOTHING.
    //
    // Field 3 does NOT mean "in motion": it held "0" through three separate
    // confirmed rotations, in different sessions and at different headings.
    // It read "5" exactly once, as a controller powered down. That resembles
    // the RT-21's documented status byte but one sample is not a mapping.
    QString pointStatus;
    QString unknownD;

    QStringList fields;         // Unknown / DeviceAdd — everything after the verb
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
//
// Unknown also covers a record that is well formed but carries a value we
// will not act on: a POINT whose heading is non-finite or outside
// [kMinHeadingDegrees, kMaxHeadingDegrees] is not a position, and is refused
// at the boundary rather than passed inward (Principle VII). The bounds are
// the ones encodeTurn refuses to send.
Record parse(const QByteArray& record);

// ── Commands (client → device) ──────────────────────────────────────────────

// Encode a SET_SWITCH. Returns an EMPTY array if either name contains a
// framing byte (US / GS / CR / LF): those names reach us from the device's
// own roster, which makes them untrusted input we would otherwise echo back
// as extra fields or extra records (Principle VII).
QByteArray encodeSelect(const QString& switchName, const QString& portName);

// Encode a TURN. Returns an EMPTY array if the rotor name contains a framing
// byte (US / GS / CR / LF) — the name reaches us from the device's own
// announcement, which makes it untrusted input (Principle VII) — or if
// `degrees` is not finite and inside [kMinHeadingDegrees, kMaxHeadingDegrees].
//
// The heading is formatted with QString::number, which is locale-independent
// by construction. QLocale::toString() is not, and a comma-decimal locale
// would put "89,0" on the wire.
QByteArray encodeTurn(const QString& rotorName, double degrees);

// Shortest angular distance between two headings, 0–180. Wrap matters: 350°
// and 10° are 20° apart, not 340°. Anything showing a commanded heading beside
// a reported one needs this.
double headingDelta(double lhs, double rhs);

// Sort key helper: orders AS-84F-2 before AS-84F-10, and anything without a
// trailing number last. DISPLAY order only — lock slots are indexed by
// announcement order, which is a different thing entirely and on this
// hardware is 1, 3, 2, 4. See GreenHeronModel::locksBySwitch().
bool displayOrderLessThan(const QString& lhs, const QString& rhs);

} // namespace GreenHeron
} // namespace AetherSDR
