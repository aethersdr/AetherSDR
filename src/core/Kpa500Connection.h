#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif

#include "Kpa500Protocol.h"

namespace AetherSDR {

// Peripheral transport for an Elecraft KPA500 linear amplifier connected over
// RS-232/USB serial. A standalone device with no FlexRadio awareness — a
// peripheral(kpa500) accessory alongside AcomConnection/SpeConnection, not an
// IRadioBackend implementor.
//
// The KPA500 speaks a text-based ^CMD[data]; protocol (see Kpa500Protocol.h)
// over a single serial port. This class owns the poll loop (fast every 200 ms,
// slow rotated every 2 s, one-shot on connect) and emits statusUpdated for
// every reply that changes the snapshot.
//
// Default serial rate is 4800 baud as per the KPA500 spec; configurable via
// RadioSetupDialog. Reconnect is automatic when the port disappears and
// reappears (e.g. USB detach/reattach).
//
// Principle II: no control method latches its own state. setOperate() sends
// the command; the applet repaints when the poll reply lands.
class Kpa500Connection : public QObject {
    Q_OBJECT

public:
    explicit Kpa500Connection(QObject* parent = nullptr);

    bool isConnected() const { return m_connected; }
    bool isResponding() const { return m_responding; }
    // "COM4 @ 4800" — for status display in RadioSetupDialog indicator.
    QString description() const;

#ifdef HAVE_SERIALPORT
    // Opens portName at baudRate (default 4800, per KPA500 spec §^BRP).
    // 8N1, no handshake. No-op if already connected.
    void connectSerial(const QString& portName, int baudRate = 4800);
#endif

    void disconnect();
    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    // Commands — no-ops when not connected (Principle II: send and let poll confirm).
    void setOperate(bool on);
    void clearFault();
    // Set fan minimum speed (0 = off, 6 = high). Clamped to [0, 6].
    void setFanSpeed(int n);

    const Kpa500::Status& lastStatus() const { return m_status; }

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    void statusUpdated(const AetherSDR::Kpa500::Status& status);
    // Transport is up but the amplifier has gone silent (or resumed). A USB-
    // serial link can outlive the amp being switched off, so this — not
    // disconnected() — is the "amp went away" signal for that topology.
    void respondingChanged(bool responding);

private slots:
#ifdef HAVE_SERIALPORT
    void onReadyRead();
    void onSerialError(QSerialPort::SerialPortError error);
#endif

private:
    void onTransportUp();
    void onTransportDown(const QString& reason = {});
    void fastPollTick();
    void slowPollTick();
    void sendRaw(const QByteArray& bytes);
    void armReconnect();
    void setResponding(bool responding);

#ifdef HAVE_SERIALPORT
    QSerialPort m_serial;
#endif

    Kpa500::FrameParser m_parser;
    Kpa500::Status      m_status;

    QString m_lastPortName;
    int     m_lastBaudRate{4800};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};

    QTimer m_reconnectTimer;
    QTimer m_fastPollTimer;
    QTimer m_slowPollTimer;

    // Rotating index into slowPollCommands() — one command per slow tick.
    int m_slowIndex{0};
    // Rotating index into fastPollCommands() — member so reconnect resets it
    // and multiple instances don't share state.
    int m_fastIndex{0};
    // One-shot connect commands: sent in sequence after transport comes up,
    // one per fast-poll tick, before regular polling starts.
    int  m_connectCmdIndex{0};
    bool m_connectCmdsDone{false};

    // Poll-silence tracking — a few missed fast-poll replies are tolerated
    // (serial latency, amp busy) before flagging silence.
    bool m_statusSeenSinceFastTick{false};
    int  m_silentTicks{0};
    bool m_responding{false};
    // ~3 s at 200 ms fast-poll cadence before flagging silence.
    static constexpr int kSilentTickLimit = 15;

    static constexpr int kFastPollMs  = 200;
    static constexpr int kSlowPollMs  = 2000;
    static constexpr int kReconnectMs = 5000;
};

}  // namespace AetherSDR
