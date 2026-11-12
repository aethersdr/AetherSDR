#pragma once

// microHAM TRIO antenna-switch controller, web protocol as observed on one
// unit from the device's own web page; the vendor documents none and the
// firmware version is not readable. Compact JSON text frames on
// ws://<host>/ui.ws: the complete state arrives on connect and as a push when
// the front panel, band data or another client changes a selection.
// Read only: this header has no command encoder.
// Pure: no socket, no clock. Tested in tests/microham_trio_protocol_test.cpp.

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace AetherSDR {
namespace MicrohamTrio {

// The real state document is about 1.1 kB with six buttons in its largest
// group; the caps sit an order of magnitude above that.
inline constexpr int kMaxFrameBytes      = 16384;
inline constexpr int kMaxButtonsPerGroup = 32;

// The id every state document carries while this session has sent no command.
inline constexpr const char* kUnsolicitedId = "00000000-0000-0000-0000-000000000000";

// What a snapshot is evidence of. The schema has no field for a relay that
// disagrees: the position is the controller's echo of its last selection,
// never relay sense. A front end may say "selected", never "connected to".
enum class PositionEvidence {
    ControllerEcho,
};

struct Button {
    QString key;          // "ant6": the value a command selects
    QString name;         // "ANTENNA6"
    QString abbreviation; // "A6"
    QString label;        // "Dummy": the operator's text, stored on the device
    bool    enabled{false};
};

struct Selector {
    QString         selected;
    QVector<Button> buttons;

    const Button* find(const QString& key) const;
    // Null when `selected` names no button: the antenna group in split.
    const Button* selectedButton() const { return find(selected); }
};

struct Toggle {
    bool            on{false};
    QVector<Button> buttons;
};

// One state document, for whichever radio holds focus. Without split the
// antenna group carries `v`, one port for TX and RX; with split it carries no
// `v` and the halves `txv` "tx_ant6" / `rxv` "rx_ant2" instead.
struct Snapshot {
    QString  id;
    Selector radio;     // which radio port holds transmit focus
    Selector antenna;   // `selected` is empty in split
    Selector rxAntenna; // RX1..RX3, the receive-only ports
    Toggle   split;
    Toggle   pa;        // amplifier loop in circuit
    Toggle   rxExt;
    QString  antennaTx; // "ant6": the port the TX half uses, split or not
    QString  antennaRx;
    PositionEvidence evidence{PositionEvidence::ControllerEcho};
};

enum class ParseError {
    None,
    Empty,              // a zero-length frame; the device sends one about every second
    TooLarge,
    NotJson,
    NotAnObject,
    MissingKey,
    UnexpectedKey,      // includes an unknown group, such as an RX extension module
    WrongType,
    NoButtons,
    TooManyButtons,
    SelectionNotAButton,
};

struct ParseResult {
    ParseError error{ParseError::None};
    QString    where;   // dotted path of the offending element, for the log
    std::optional<Snapshot> snapshot;
};

// Parse one text frame. Strict: every group, key and type must be exactly the
// observed schema, or there is no snapshot. A schema that moved is unreadable,
// never half-read.
ParseResult parse(const QByteArray& frame);

// The keep-alive: a text frame holding the four bytes `ping`, no quotes. The
// device answers with a text frame `pong`.
QByteArray encodeKeepAlive();

// First byte of a WebSocket frame as the device sends it: FIN plus opcode.
inline constexpr quint8 kTextFrame = 0x81;
inline constexpr quint8 kPongFrame = 0x8a;

enum class FrameKind {
    Unknown,         // anything else, including an unreadable state
    KeepAliveReply,  // text `pong`
    ServerPong,      // an empty pong control frame; the device sends one about every second
    State,           // a state document
};

struct Frame {
    FrameKind   kind{FrameKind::Unknown};
    ParseResult state;   // filled for any text frame that is not `pong`
};

// Classify one received frame. Every kind shows the peer is alive; only State
// carries a snapshot.
Frame classify(quint8 firstByte, const QByteArray& payload);

// The one long-lived session. The device serves a single session: a second
// client evicts the first.
enum class Link {
    NeverConnected,
    Held,
    Evicted,   // the peer closed it: another client took the session
    Failed,    // no connection, no upgrade, or an unreadable state
};

// Timing parameters. None is a device constant; all are the caller's to set.
struct Policy {
    qint64 keepAliveIntervalMs{5000};  // the device's own web page uses 5 s
    qint64 sessionDeadAfterMs{5000};   // silent, or a keep-alive unanswered, this long (page: 10 s)
    qint64 failureBackoffMs{5000};     // doubles per consecutive failure
    qint64 maxBackoffMs{600000};
};

// Everything decideSession() looks at. Ages are measured by the caller at the
// moment of the call; a negative age is treated as unusable.
struct State {
    bool releasedForBrowser{false};   // the operator gave the session to the web page
    Link   link{Link::NeverConnected};
    qint64 lastMessageAgeMs{0};       // any frame received on the held session
    std::optional<qint64> lastKeepAliveAgeMs;
    std::optional<qint64> unansweredKeepAliveAgeMs;   // oldest keep-alive with no reply yet
    std::optional<qint64> lastConnectAgeMs;
    int    consecutiveFailures{0};
    bool   hasSnapshot{false};        // a state document arrived on this session
};

// Held, yet silent or owing a keep-alive reply for longer than sessionDeadAfterMs.
bool sessionDead(const State& state, const Policy& policy = {});

enum class SessionAction {
    Wait,             // waitMs 0: ask again when the state changes
    Connect,          // drop any socket and open a new session
    Disconnect,
    SendKeepAlive,    // encodeKeepAlive()
};

enum class SessionReason {
    FirstConnect,
    Evicted,
    RetryAfterFailure,
    BackoffAfterFailure,
    SessionDead,
    Released,
    AwaitingState,
    KeepAliveDue,
    KeepAliveNotDue,
};

struct SessionDecision {
    SessionAction action{SessionAction::Wait};
    SessionReason reason{SessionReason::KeepAliveNotDue};
    qint64        waitMs{0};
};

// What to do with the session now. An eviction is never retaken: connecting
// again would evict the other client in turn (the device's own web page, most
// often), so only the operator's Connect takes the session back. A failure is
// retried after a doubling backoff; a session released for the browser is
// closed and left alone.
SessionDecision decideSession(const State& state, const Policy& policy = {});

} // namespace MicrohamTrio
} // namespace AetherSDR
