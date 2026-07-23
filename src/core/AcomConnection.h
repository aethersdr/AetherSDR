#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif

#include "AcomProtocol.h"

namespace AetherSDR {

// Peripheral transport for an ACOM S-series amplifier (500S/600S/700S/1200S/
// 2020S) — a standalone RS-232 device with no FlexRadio awareness at all, so
// this is a peripheral(acom) accessory alongside PgxlConnection/TgxlConnection,
// not an IRadioBackend implementor. See
// docs/architecture/acom-600s-amplifier-design.md for the full design note.
//
// The wire protocol is binary and transport-agnostic — the exact same bytes
// flow whether the peer is a local COM port or a raw-mode ser2net TCP proxy —
// so a single Acom::FrameParser instance decodes either transport. Only one
// transport is active at a time, selected by which connect method is called.
class AcomConnection : public QObject {
    Q_OBJECT

public:
    explicit AcomConnection(QObject* parent = nullptr);

    bool isConnected() const { return m_connected; }
    QString description() const;  // "COM4" or "192.168.1.52:7000", for status display
    // "SERIAL" / "NETWORK" for the applet's compact source label — derived from
    // the LIVE transport (m_mode), never the persisted ConnectionMode setting.
    // The two can diverge: switching the Radio Setup mode combo persists the
    // setting without disconnecting, so a serial link that later auto-reconnects
    // would be mislabelled "NETWORK" if the label came from the setting.
    QString sourceLabel() const;

#ifdef HAVE_SERIALPORT
    // Fixed 9600 8N1, no handshake — mandated by the amplifier's own spec,
    // not user-configurable.
    void connectSerial(const QString& portName);
#endif
    // Network mode assumes a RAW TCP proxy (e.g. ser2net `connection type: raw`).
    // A telnet-mode proxy will IAC-escape byte 0xFF, which appears legitimately
    // in this protocol (e.g. errorCode 0xFF = "no fault") and will corrupt the
    // stream.
    void connectNetwork(const QString& host, quint16 port);
    void disconnect();

    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    // Commands (host -> amp). No-ops when not connected.
    void setOperate(bool on);
    void powerOff();
    void clearFaults();

    const Acom::Telemetry& lastTelemetry() const { return m_lastTelemetry; }

    // Effective model tier right now — starts at "600S" (our one confirmed
    // model) on every fresh connect, and only ever moves UP (auto-ranging;
    // see modelChanged) or gets confirmed by a SystemConfig reply. Never
    // reset mid-session, only on reconnect. See design doc §6.
    QString currentModel() const { return m_currentModel; }

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    void telemetryUpdated(const AetherSDR::Acom::Telemetry& telemetry);
    // Raw 10-word (20-byte) error-code bitmask from address 0x21. Per-bit
    // decode into named faults is a documented follow-up (see AcomProtocol.h);
    // the single "currently displayed" fault is already available via
    // telemetryUpdated()'s Telemetry::errorCode.
    void rawErrorCodesUpdated(const QByteArray& tenWords);
    // Fires once on every connect ("default"), again if a SystemConfig reply
    // confidently identifies the model ("confirmed"), and again any time
    // observed forward power outgrows the current tier ("auto-scaled"). The
    // GUI re-applies gauge ranges each time rather than just once at connect.
    void modelChanged(const QString& modelName, const QString& reason);
    // One-shot PCB versions/type/serial from a 0x11 reply, surfaced mainly
    // for the diagnostic tooltip and for users to report an unrecognized
    // amplifierType back to the project (see design doc §6).
    void systemConfigReceived(const AetherSDR::Acom::SystemConfig& config);

private slots:
    void onReadyRead();

private:
    enum class Mode { None, Serial, Network };

    void onTransportUp();
    void onTransportDown();
    void onTransportError(const QString& errorString);
    void onFrameReceived(const Acom::Frame& frame);
    void teardownDevice();
    void sendRaw(const QByteArray& frame);
    void armReconnect();
    void requestSystemConfig();       // resets attempt counter, sends first request
    void sendSystemConfigRequest();   // one attempt; re-armed by the retry timer
    void maybeAutoRangeUp(const Acom::Telemetry& t);

    QIODevice*    m_device{nullptr};
    QTcpSocket    m_socket;
#ifdef HAVE_SERIALPORT
    QSerialPort*  m_serialPort{nullptr};
#endif

    Acom::FrameParser m_parser;
    Acom::Telemetry   m_lastTelemetry;

    Mode      m_mode{Mode::None};
    QString   m_lastSerialPort;
    QString   m_lastHost;
    quint16   m_lastPort{0};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};
    bool m_frameSeenSinceTick{false};

    QTimer m_reconnectTimer;
    // Re-arms telemetry push if the amp goes quiet (it doesn't confirm the
    // enable command stuck) — same keepalive cadence as the open-source
    // reference client this protocol was cross-checked against (see
    // THIRD_PARTY_LICENSES).
    QTimer m_keepaliveTimer;

    QString m_currentModel{QStringLiteral("600S")};  // see currentModel()/modelChanged
    QTimer  m_systemConfigRetryTimer;
    int     m_systemConfigAttempts{0};
    static constexpr int kSystemConfigMaxAttempts = 5;  // spec's own retry ceiling (§1)

    // Latched so an undersized-frame diagnostic is logged once on the
    // transition into failure, not on every ~10 Hz frame — a firmware variant
    // whose frames are permanently too short would otherwise flood the log
    // indefinitely. Reset on a successful decode (recovery) and on reconnect.
    bool m_telemetryDecodeFailed{false};
    bool m_errorCodesDecodeFailed{false};

    // Auto-range debounce: a single checksum-valid-but-corrupt frame with a
    // spurious forwardPowerW would otherwise ratchet the gauge scaling up
    // irreversibly for the whole session. Require the reading to clear the
    // current tier's ceiling on this many consecutive frames before bumping.
    static constexpr int kAutoRangeConsecutiveFrames = 2;
    QString m_pendingAutoRangeTier;
    int     m_autoRangeStreak{0};
};

}  // namespace AetherSDR
