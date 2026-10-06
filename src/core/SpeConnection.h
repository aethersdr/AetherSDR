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
#include "SpeLegacyProtocol.h"
#include "SpeProtocol.h"

namespace AetherSDR {

// Peripheral transport for SPE Expert amplifiers (1.3K/1.5K/2K-FA, and the
// original 1K-FA via Spe::Variant::Legacy1k — see setVariant()): an
// accessory alongside Acom/Pgxl/TgxlConnection, not an IRadioBackend. Design:
// docs/architecture/spe-expert-amplifier-design.md. The same bytes flow over a
// local COM port or a ser2net TCP proxy, so one Spe::FrameParser serves
// whichever transport is active. The SPE only answers polls, so this class
// owns the 0x90 Status poll loop (kPollIntervalMs) and emits statusUpdated.
class SpeConnection : public QObject {
    Q_OBJECT

public:
    explicit SpeConnection(QObject* parent = nullptr);
    // Releases the 1K-FA's DTR power line before the port closes — see
    // teardownDevice(). Emits nothing.
    ~SpeConnection() override;

    // How the last powerOn() went, for the applet. Network only can fall
    // short: a proxy that refuses RFC 2217 may not move the line, and a
    // bridge that never answers (raw ser2net, plain serial-to-Ethernet
    // converters such as a Waveshare) cannot move it at all.
    enum class PowerOnResult { Sent, ProxyRefused, NoLineControl };
    Q_ENUM(PowerOnResult)

    bool isConnected() const { return m_connected; }
    QString description() const;  // "COM4" or "192.168.1.52:64002", for status display
    // "SERIAL" / "NETWORK" for the applet's compact source label — derived
    // from the LIVE transport, never the persisted ConnectionMode setting
    // (same divergence rationale as AcomConnection::sourceLabel()).
    QString sourceLabel() const;

    // Selects the wire protocol. Takes effect on the next connect; the
    // newer family self-identifies but the original 1K-FA cannot be
    // detected, so the operator's Peripherals choice is passed in here.
    void setVariant(Spe::Variant variant) { m_variant = variant; }
    Spe::Variant variant() const { return m_variant; }
    // The variant latched by the current (or last) connect.
    Spe::Variant activeVariant() const { return m_activeVariant; }

#ifdef HAVE_SERIALPORT
    // 8N1, no handshake, at Spe::serialBaud(variant): 115200 for the newer
    // family (it auto-adapts to lower speeds, spec §1), 9600 for the 1K-FA.
    void connectSerial(const QString& portName);
#endif
    // Expects a ser2net proxy in raw or telnet mode (both verified on a 1.5K-FA);
    // the LCD parser handles raw and doubled-IAC frames. A Status checksum byte of
    // 0xFF is dropped and re-polled 100 ms later. powerOn() needs RFC 2217
    // (`accepter: telnet(rfc2217=true),<port>`); other modes still monitor and
    // send keys, and powerOn() reports the gap. See design note §4.
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

    // Power ON via a hardware pulse on the connector's control lines (works while
    // the amp is silent): RFC 2217 DTR/RTS in network mode (needs
    // `accepter: telnet(rfc2217=true),<port>`), local lines in serial mode. The
    // pulse is always sent; completion reports what the peer agreed to (DO / DONT
    // / no answer). Timing per Spe::Rfc2217 and design note §4. No-op while a
    // pulse is in progress.
    void powerOn();

    const Spe::Status& lastStatus() const { return m_lastStatus; }

    // Model ID from the last Status reply ("13K"/"15K"/"20K"/"10K"), empty until
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
    void powerOnReported(AetherSDR::SpeConnection::PowerOnResult result);

private slots:
    void onReadyRead();

private:
    enum class Mode { None, Serial, Network };

    void onTransportUp();
    void onTransportDown();
    void onTransportError(const QString& errorString);
    void onFrameReceived(const Spe::Frame& frame);
    void onLegacyFrameReceived(const Spe::Legacy::Frame& frame);
    void acceptStatus(const Spe::Status& status);
    void reportPowerOnOutcome();
    bool isLegacy() const { return m_activeVariant == Spe::Variant::Legacy1k; }
    void teardownDevice();
    void sendRaw(const QByteArray& packet);
    void armReconnect();
    void pollTick();
    void powerOnStep();
    void setControlLines(bool dtr, bool rts);  // transport-appropriate DTR/RTS
    // Lets queued bytes (RCU_OFF, a key) reach the wire before a close
    // discards them; bounded, as in FlexControlManager.
    void drainWrites();
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
    Spe::Legacy::FrameParser m_legacyParser;
    Spe::Status      m_lastStatus;

    // m_variant is the configured choice; m_activeVariant is latched at
    // connect so a settings change mid-session cannot switch parsers under
    // a live stream.
    Spe::Variant m_variant{Spe::Variant::Expert};
    Spe::Variant m_activeVariant{Spe::Variant::Expert};
    // 1K-FA only: ON raised DTR on THIS connection's transport — always on
    // a local port, over the network only if the proxy answered the RFC 2217
    // request (a raw bridge moves no line). Network teardown releases the
    // line only when this is set; a local port is released unconditionally.
    // Cleared on every connect: the line policy (design note §12) is that
    // DTR is held only while AetherSDR has the port open, so a serial
    // connect always starts with DTR low and never powers the amp on.
    bool m_legacyDtrRaised{false};
    // SWITCH OFF sends the OFF key first and releases DTR this much later,
    // so the key is on the wire (~7 ms at 9600 baud) before the line drops.
    QTimer m_offReleaseTimer;
    static constexpr int kLegacyOffReleaseMs = 150;

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
    // Lost-reply fallback while a request is in flight. Must stay far above the
    // worst round trip (~390 ms to serialize a frame at 9600 baud): replies carry
    // no request id, so a reply arriving after the fallback is credited to the
    // retry and two requests stay on the wire until reset(). Not a multiple of
    // kPollIntervalMs (avoids phase lock when it free-runs). Keep below
    // kLcdStaleTimeoutMs (2400): a vanished reply's retry needs time to land
    // before the front-panel keys gate.
    static constexpr int kLcdLostReplyMs = 2000;
    // Retry pause after a complete display frame fails validation. Short so a
    // mostly-corrupted stream still lands a clean frame inside the staleness
    // window; each retry costs a full received frame, so it can't run away.
    // Intentionally below kLcdPollIntervalMs; on slow links it costs wire share
    // (hence the design note's ≥57600 proxy recommendation).
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
    // The 1K-FA streams on its own once RCU_ON lands; while no Status has
    // arrived for this many ticks (~1 s), RCU_ON is re-sent — it is lost
    // if the amp is still booting or was power-cycled.
    static constexpr int kLegacyRcuRetryTicks = 10;
};

}  // namespace AetherSDR
