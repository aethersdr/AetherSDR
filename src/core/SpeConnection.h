#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif

#include "SpeLcdScheduler.h"
#include "SpeProtocol.h"

namespace AetherSDR {

// Peripheral transport for an SPE Expert linear amplifier (1.3K-FA/1.5K-FA/
// 2K-FA) — a standalone USB/RS-232 device with no FlexRadio awareness at
// all, so this is a peripheral(spe) accessory alongside AcomConnection/
// PgxlConnection/TgxlConnection, not an IRadioBackend implementor. See
// docs/architecture/spe-expert-amplifier-design.md for the full design note.
//
// The wire protocol is transport-agnostic — the exact same bytes flow
// whether the peer is a local COM port or a raw-mode ser2net TCP proxy —
// so a single Spe::FrameParser instance decodes either transport. Only one
// transport is active at a time, selected by which connect method is called.
//
// Unlike the ACOM (which pushes telemetry continuously), the SPE only ever
// speaks when spoken to: the host polls the Status string with command 0x90.
// This class owns that poll loop (kPollIntervalMs) and emits statusUpdated
// for every valid reply.
class SpeConnection : public QObject {
    Q_OBJECT

public:
    explicit SpeConnection(QObject* parent = nullptr);

    bool isConnected() const { return m_connected; }
    QString description() const;  // "COM4" or "192.168.1.52:64002", for status display
    // "SERIAL" / "NETWORK" for the applet's compact source label — derived
    // from the LIVE transport, never the persisted ConnectionMode setting
    // (same divergence rationale as AcomConnection::sourceLabel()).
    QString sourceLabel() const;

#ifdef HAVE_SERIALPORT
    // 115200 8N1, no handshake — the amplifier auto-adapts to lower speeds
    // (spec §1), so the maximum documented rate is used and not made
    // user-configurable.
    void connectSerial(const QString& portName);
#endif
    // Network mode expects a ser2net-style TCP proxy in either raw or
    // telnet mode — both verified against real 1.5K-FA hardware (the
    // validation station runs telnet mode). The LCD parser accepts both raw
    // binary frames and telnet's doubled-IAC form and validates their 16-bit
    // checksum. A rare Status checksum byte of 0xFF is still dropped and
    // simply re-polled 100 ms later. Telnet mode is also what the remote
    // power-ON pulse needs — it drives the proxy's DTR/RTS lines via RFC 2217
    // COM-port control, so ser2net must run the port as
    // `accepter: telnet(rfc2217=true),<port>`. A raw or plain-telnet port
    // still monitors and sends keystrokes fine; only powerOn() is affected,
    // and it detects and reports that case rather than assuming. See the
    // design note §4.
    void connectNetwork(const QString& host, quint16 port);
    void disconnect();

    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    // Commands (host -> amp). Each is a front-panel keystroke; the amplifier
    // echoes an ACK or replies with a Status string. No-ops when not
    // connected.
    void sendKey(Spe::Key key);
    void toggleOperate() { sendKey(Spe::Key::Operate); }
    void cyclePowerLevel() { sendKey(Spe::Key::Power); }
    void tune() { sendKey(Spe::Key::Tune); }
    void switchOff() { sendKey(Spe::Key::SwitchOff); }

    // Remote LCD mirroring: while enabled (and connected) the amplifier's
    // display is polled with the 0x80 request — each reply schedules the
    // next request kLcdPollIntervalMs later — and every decoded refresh
    // arrives via lcdFrameReceived. Driven by the
    // applet's floating state — the docked rail has no room for the LCD,
    // so polling it there would be pure link noise.
    void setLcdPolling(bool on);

    // Power the amplifier ON — a hardware pulse on the serial connector's
    // control lines, not a protocol command, so it works while the amp is
    // silent. Network mode drives the proxy's DTR/RTS via RFC 2217, which
    // needs ser2net running the port as
    // `accepter: telnet(rfc2217=true),<port>`; serial mode drives the local
    // lines directly. The pulse is always sent — a proxy that ignores
    // COM-port control just discards it — but the completion message reports
    // what the peer actually agreed to (DO / DONT / no answer at all) rather
    // than assuming it landed. The pulse sequence and timing are carried verbatim
    // from the field-proven reference application (see Spe::Rfc2217 and
    // design note §4). No-op while a pulse is already in progress.
    void powerOn();

    const Spe::Status& lastStatus() const { return m_lastStatus; }

    // Model ID from the last Status reply ("13K"/"15K"/"20K"), empty until
    // the first reply arrives. The SPE reports its identity in every Status
    // string, so — unlike AcomConnection — there is no auto-ranging or
    // detection heuristic here at all.
    QString currentModelId() const { return m_currentModelId; }

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    void statusUpdated(const AetherSDR::Spe::Status& status);
    void lcdFrameReceived(const AetherSDR::Spe::Lcd::Frame& frame);
    // True only after a checksum-valid LCD reply, and false again after
    // kLcdStaleTimeoutMs without one, or whenever LCD polling/transport
    // stops. The floating menu keys use this independently of Status
    // liveness.
    void lcdFreshChanged(bool fresh);
    // Fires on the first Status reply of a connection and again if the
    // reported ID ever changes (in practice: never mid-session). The GUI
    // applies gauge ranges and model-dependent layout from this.
    void modelChanged(const QString& modelId);
    // The transport is up but the amplifier has stopped answering polls
    // (or resumed). With ser2net the TCP link outlives the amplifier being
    // switched off, so this — not disconnected() — is the "amp went away"
    // signal for that topology.
    void respondingChanged(bool responding);

private slots:
    void onReadyRead();

private:
    enum class Mode { None, Serial, Network };

    void onTransportUp();
    void onTransportDown();
    void onTransportError(const QString& errorString);
    void onFrameReceived(const Spe::Frame& frame);
    void teardownDevice();
    void sendRaw(const QByteArray& packet);
    void armReconnect();
    void pollTick();
    void powerOnStep();
    void setControlLines(bool dtr, bool rts);  // transport-appropriate DTR/RTS
    // Executes a scheduler decision: send the 0x80 request and/or re-arm
    // m_lcdTimer with the interval for the role the scheduler assigned it.
    void applyLcdEffect(const Spe::LcdScheduler::Effect& effect);
    void setLcdFresh(bool fresh);

    QIODevice*    m_device{nullptr};
    QTcpSocket    m_socket;
#ifdef HAVE_SERIALPORT
    QSerialPort*  m_serialPort{nullptr};
#endif

    Spe::FrameParser m_parser;
    Spe::Status      m_lastStatus;

    Mode      m_mode{Mode::None};
    QString   m_lastSerialPort;
    QString   m_lastHost;
    quint16   m_lastPort{0};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};

    QTimer m_reconnectTimer;
    // Status poll loop — the spec allows "several times every second". 10/s
    // matches the applets' own 10 Hz readout refresh (kMeterReadoutUpdateMs),
    // so polling faster would only burn link bandwidth on frames the GUI
    // never renders; the field-proven reference application polled at 300 ms
    // and its bar visibly stair-stepped, which this rate fixes.
    QTimer m_pollTimer;
    static constexpr int kPollIntervalMs = 100;

    // Display-request pacing is decided entirely by the I/O-free
    // Spe::LcdScheduler (see SpeLcdScheduler.h): requests are single-file
    // — at most one in flight, at most one timer armed — with every
    // trigger path (idle cadence, keystroke ACK, corrupted-frame retry,
    // lost-reply fallback) flowing through the same gate. m_lcdTimer is
    // that ONE timer; applyLcdEffect() arms it with the interval for
    // whichever role the scheduler assigned. The no-overlap property is
    // unit-tested in spe_protocol_test.
    Spe::LcdScheduler m_lcdScheduler;
    QTimer m_lcdTimer;
    QTimer m_lcdStaleTimer;
    bool   m_lcdWanted{false};
    bool   m_lcdFresh{false};
    // The IDLE GAP between a display reply and the next request, not a
    // free-running period — the effective cadence is gap + round trip +
    // the link's serialization time for the 371-byte frame (~32 ms at
    // 115200, ~193 ms at 19200), so a slow link stretches the cadence
    // instead of piling requests up, and the amp is never asked to
    // interleave display blocks. (At ≤9600 the 100 ms Status poll alone
    // nearly saturates the wire — see the design note §11's proxy baud
    // recommendation.)
    static constexpr int kLcdPollIntervalMs = 250;
    // Lost-reply fallback, armed while a request is in flight. Sized so
    // far above the worst plausible round trip (a 9600 baud proxy serial
    // side spends ~390 ms serializing the frame alone) that a reply
    // arriving AFTER it is implausible rather than merely unlikely.
    //
    // That margin is load-bearing, and it is the only mitigation the
    // protocol permits: there is no request id — buildRequest() is a
    // fixed packet and the reply carries no sequence field — so a reply
    // that does arrive after the fallback CANNOT be told from the
    // retry's own reply. The scheduler then credits it to the wrong
    // request and two requests stay on the wire until the next reset().
    // Bookkeeping cannot fix that: a counter that swallows the late
    // reply swallows a genuinely-retried one just as often, freezing the
    // mirror instead. Widening the window is the fix.
    //
    // Also deliberately NOT a multiple of kPollIntervalMs: when replies
    // stop entirely this timer is the only thing pacing requests and it
    // free-runs, which is exactly the evenly-dividing-period condition
    // that phase-locked the original 600 ms cadence to the Status poll.
    //
    // Interacts with kLcdStaleTimeoutMs (2400 ms): a retry at 2000 ms
    // has ~400 ms to land a frame before the FRONT PANEL keys gate, so a
    // single VANISHED reply can now brush the gate where 1000 ms did
    // not. Corrupted replies — the field case — take the 80 ms
    // kLcdRetryGapMs path instead and are unaffected. Keep this below
    // kLcdStaleTimeoutMs if either constant moves.
    static constexpr int kLcdLostReplyMs = 2000;
    // Retry pause after a display frame arrives complete but fails
    // validation. Short enough that a mostly-corrupted mid-transmit
    // stream still lands a clean frame within the staleness window
    // whenever one gets through at all; long enough that the retry stream
    // (each retry provoked by a full received frame) stays well under the
    // wire's capacity even at 115200 with Status polling. Deliberately
    // NOT bounded below by kLcdPollIntervalMs: a corrupted stream is
    // answered FASTER than a healthy one, because the mirror needs only
    // one clean frame to stay live and the retry cannot run away — each
    // one costs a full received frame's serialization time. The trade is
    // wire share on a slow link, which is what the design note's ≥57600
    // proxy recommendation covers.
    static constexpr int kLcdRetryGapMs = 80;
    // Absolute, deliberately decoupled from the poll gap: it must cover a
    // full lost frame plus a retry on the slowest plausible link (a 9600
    // baud proxy serial side spends ~390 ms per display frame) AND the
    // amplifier's own quiet spells — it stops serving the display for a
    // moment around OPERATE/STANDBY relay transitions — so routine events
    // never flap the gate. On a fast link the margin only calms things.
    static constexpr int kLcdStaleTimeoutMs = 2400;

    QString m_currentModelId;

    // Power-ON pulse state machine (see powerOn()). -1 = idle.
    QTimer m_powerOnTimer;
    int    m_powerOnStep{-1};
    // Whether the network peer accepted RFC 2217 COM-port control, learned
    // from its reply to WILL COM-PORT-OPTION. Network mode only; a local
    // serial port drives its own lines and needs no negotiation.
    Spe::Rfc2217::OptionReply m_comPortOption{Spe::Rfc2217::OptionReply::None};
    bool m_rfc2217NegotiationPending{false};
    // Last 2 bytes of the previous network read — prepended to the next scan
    // so a DO/DONT reply split across TCP segments is still seen (the
    // sequence is 3 bytes, so 2 carried bytes always suffice).
    QByteArray m_rfc2217Tail;

    // Responding/silent tracking: a poll counts as unanswered if no Status
    // frame arrived since the previous tick. A few misses are tolerated
    // (serial latency, a busy amp, a marginal link) before flagging silence.
    bool m_statusSeenSinceTick{false};
    int  m_silentPolls{0};
    bool m_responding{false};
    static constexpr int kSilentPollLimit = 30;  // ~3 s at the 100 ms poll cadence
};

}  // namespace AetherSDR
