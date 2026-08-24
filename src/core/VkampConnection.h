#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include "VkampProtocol.h"

namespace AetherSDR {

// Peripheral transport for a VK3AMP RF amplifier -- a standalone TCP-
// controlled device with no FlexRadio awareness at all, so this is a
// peripheral(vkamp) accessory alongside PgxlConnection/TgxlConnection/
// AcomConnection, not an IRadioBackend implementor. v1 is TCP control/
// status + UDP telemetry only -- serial is a genuinely different wire
// format (VkampProtocol.h's own doc comment, design doc Section 3.3)
// deferred to a later phase. See
// docs/architecture/vkamp-amplifier-design.md for the full design note.
class VkampConnection : public QObject {
    Q_OBJECT

public:
    explicit VkampConnection(QObject* parent = nullptr);

    bool isConnected() const { return m_connected; }
    QString description() const;  // "192.168.1.50:5005", for status display

    void connectNetwork(const QString& host, quint16 port = 5005, quint16 udpPort = 5010);
    void disconnect();
    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    const Vkamp::Status& lastStatus() const { return m_lastStatus; }
    bool isResetting() const { return m_resetting; }

    // Commands (host -> amp). No-ops (logged) when not connected.
    void setBypass(bool on);
    void setCoolingOverride(bool on);
    // Refuses (logs, no-op) while the amp is bypassed -- a real-hardware
    // safety finding (design doc Section 5): commanding a rail change while
    // bypassed was observed live pulling the amp's supply toward ~0V.
    void setVoltage(bool low);
    void selectAntenna(int port);  // 1-3; out of range is a no-op, logged
    // Hold-to-confirm: repeats "23" for kResetHoldSeconds, emitting
    // resetProgress() throughout, then resetFinished(). No-op if a reset is
    // already in progress or not connected -- the caller (VkampApplet, via
    // MainWindow wiring) is expected to have already shown its own confirm
    // dialog before calling this.
    void startReset();

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    void statusUpdated(const AetherSDR::Vkamp::Status& status);
    void telemetryUpdated(const AetherSDR::Vkamp::Telemetry& telemetry);
    // UDP telemetry has gone quiet while the TCP link is still healthy --
    // i.e. the amp stopped transmitting (design doc Section 3.2: telemetry
    // is TX-gated and the amp is silent at all other times). Consumers must
    // treat the last frame as expired rather than keep displaying it; see
    // m_telemetryStallTimer.
    void telemetryStalled();
    void resetProgress(double remainingSeconds);
    void resetFinished();

private slots:
    void onTcpReadyRead();
    void onUdpReadyRead();

private:
    void onTransportUp();
    void onTransportDown();
    void onTransportError(const QString& errorString);
    void onStatusReceived(const Vkamp::Status& status);
    // Fire-and-forget over TCP, gated on connection state,
    // kMinCommandIntervalMs, and an in-progress reset hold. Returns false
    // (and logs) if the command was dropped rather than sent -- callers don't
    // currently act on this, but it mirrors the companion project's own
    // "never silently swallow a dropped command" stance.
    //
    // `isResetHold` is the one exemption from the reset gate, for the hold
    // stream itself. Everything else -- keepalive polls and user-driven
    // commands alike -- is held back for the duration: this protocol has no
    // correlation ID at all (design doc Section 6), so at most one command
    // may be unconfirmed at a time, and a foreign byte pair landing inside
    // the "23" stream also risks restarting the amp's own ~9-12s hold
    // counter, which would make a reset appear to run to completion without
    // ever taking effect.
    bool sendCommand(const QByteArray& code, bool isResetHold = false);
    // Pokes the telemetry port so the amp keeps streaming to this (ip, port)
    // for the life of the connection (design doc Section 3.2). No-op until
    // m_resolvedHost is known; logs rather than swallowing a failed write.
    void sendTelemetryTrigger();
    void armReconnect();

    QTcpSocket m_socket;
    QUdpSocket m_udpSocket;

    // The address the TCP socket actually resolved to, captured on connect
    // and used for every datagram. NOT QHostAddress(m_lastHost): that ctor
    // parses only a literal address, so a hostname (or an mDNS name) yields
    // a null QHostAddress and writeDatagram() silently fails -- while
    // connectToHost() resolves the same string fine, leaving the UI
    // reporting a healthy connection with telemetry that never arrives.
    // Nothing in the Peripherals row restricts the field to a dotted quad.
    QHostAddress m_resolvedHost;

    Vkamp::StatusStreamParser m_parser;
    Vkamp::Status m_lastStatus;

    QString m_lastHost;
    quint16 m_lastPort{5005};
    quint16 m_lastUdpPort{5010};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};

    // Without an explicit watchdog, an unreachable/wrong IP falls back to
    // the OS's own default TCP connect timeout -- as long as ~21s on
    // Windows -- before connectionFailed() ever fires, which is what made
    // Connect feel hung rather than just slow. Same epoch-guarded
    // QTimer::singleShot pattern as DxClusterClient/WaveformInstaller/
    // ProfileTransfer's own kConnectTimeoutMs (#2380: a stale timeout from
    // attempt N must never abort a later attempt that already succeeded).
    //
    // 18000ms, not a smaller codebase-convention value -- ported from the
    // companion vkamp_client.py's own VKAmp.__init__ default (timeout=18.0),
    // whose own doc comment explains why: this amp's network stack only
    // answers BROADCAST ARP "who-has" requests, never unicast ones, but
    // Windows' neighbor-cache reconfirmation tries unicast probes against a
    // stale ARP entry first and only falls back to broadcast once those go
    // unanswered -- confirmed via live capture to take anywhere from ~9s to
    // >12s depending on how stale the cached entry was. A shorter timeout
    // (this shipped at 5000ms, then 10000ms, before this fix) cuts that
    // escalation off mid-flight on every cold connect -- it then works fine
    // on retry only because Windows has since finished the broadcast
    // resolution and cached it, which is exactly the "works on the second
    // click" symptom this was chasing. 18s gives the whole dance room to
    // finish inside a single attempt, with margin.
    static constexpr int kConnectTimeoutMs = 18000;
    int m_connectEpoch{0};

    QTimer m_reconnectTimer;
    // The amp is silent during genuine idle time (design doc Section 6) --
    // this keepalive is the ONLY thing that produces a status reply while
    // idle, not a health check by itself.
    QTimer m_keepaliveTimer;
    static constexpr int kKeepaliveIntervalMs = 15000;
    // Watches for a genuinely dead link (design doc Section 6) -- set
    // generously above real observed idle jitter, matching the companion
    // project's own hard-won figure for this amp's connection.
    QTimer m_staleStatusTimer;
    static constexpr int kStaleStatusTimeoutMs = 90000;
    // UDP telemetry needs periodic re-triggering on the SAME socket/port
    // (design doc Section 3.2) -- a fresh socket each time risks losing
    // telemetry right as a TX starts.
    QTimer m_telemetryTriggerTimer;
    static constexpr int kTelemetryRetriggerMs = 3000;
    // Fires when telemetry stops arriving on an otherwise healthy link,
    // which for this amp is the normal end of a transmission rather than a
    // fault -- so it drives telemetryStalled(), not a disconnect. Comfortably
    // above kTelemetryRetriggerMs so a frame arriving only in reply to a
    // retrigger never trips it.
    QTimer m_telemetryStallTimer;
    static constexpr int kTelemetryStallMs = 5000;

    // Protects the amp's relay/PSU from a rapid-fire command burst -- a real
    // live finding from the companion project: 5 commands landing within
    // ~20ms of each other sagged the reported supply voltage in lockstep and
    // 2 of them silently failed to apply. Independent of any per-command
    // confirmation bookkeeping, which this GUI-facing layer doesn't need
    // (unlike the companion project's own diagnostic-logging worker):
    // commands here are fire-and-forget, and the applet just reflects live
    // status as it arrives, same as AcomConnection's own precedent.
    QElapsedTimer m_lastCommandTimer;
    static constexpr int kMinCommandIntervalMs = 250;

    // Reset hold-to-confirm state (design doc Section 4: the amp needs "23"
    // repeated for ~9-12s before it registers).
    QTimer* m_resetTimer{nullptr};
    bool m_resetting{false};
    double m_resetRemaining{0.0};
    static constexpr double kResetHoldSeconds = 10.0;
    static constexpr double kResetIntervalSeconds = 1.0;
};

}  // namespace AetherSDR
