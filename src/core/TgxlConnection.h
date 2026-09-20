#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QTimer>
#include <QMap>
#include <QString>

namespace AetherSDR {

// Direct TCP connection to a 4O3A Tuner Genius XL on port 9010.
// Provides manual relay control (C1/L/C2) via the TGXL's native protocol,
// which is independent of the FlexRadio on port 4992.
//
// Protocol format (same style as SmartSDR):
//   C<seq>|<command>\n          — client command
//   R<seq>|<code>|<body>\n      — TGXL response
//   S0|state key=val ...\n      — unsolicited state push
//   M|<text>\n                  — alert text; empty body clears it
//   V<version>\n                — version line on connect
//
// Reverse-engineered from 4O3A TGXL management app pcap (#469).
class TgxlConnection : public QObject {
    Q_OBJECT

public:
    explicit TgxlConnection(QObject* parent = nullptr);

    bool isConnected() const { return m_connected; }
    QString version() const { return m_version; }
    QString peerAddress() const { return m_socket.peerAddress().toString(); }
    quint16 peerPort() const { return m_socket.peerPort(); }

    void connectToTgxl(const QString& host, quint16 port = 9010);
    void disconnect();

    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    // Manual relay adjustment: relay 0=C1, 1=L, 2=C2; direction +1 or -1
    void adjustRelay(int relay, int direction);

    // Native autotune over the direct port-9010 channel. The TGXL drives
    // radio PTT via its hardware interlock cable, so no client-side keying
    // is required. Bypasses the firmware's `tgxl autotune` command path
    // (broken in firmware 4.2 — see issue tracker for "TUNE button on TGXL").
    void requestAutotune();

    // Send an arbitrary command to the TGXL (e.g. "activate ant=2")
    quint32 sendCommand(const QString& cmd);

    // Poll fast only while the transmitter is keyed.
    //
    // Rates measured against a live TGXL on 1.2.17: the transport sustains
    // 129 Hz request-response (7 ms median round trip), and the reported
    // value changes every 17 ms median (~59 Hz), so ~60 Hz is the point
    // past which polling returns duplicate frames. Receiving needs none of
    // that -- nothing is moving -- so it drops to 4 Hz.
    //
    // Driven from the ptt fields in the device's own status frames, so it
    // needs no wiring to the radio. The cost is that a transmission is
    // noticed up to one RX poll late (250 ms); setTransmitting() lets a
    // caller that already knows switch the rate up with no delay.
    void setTransmitting(bool tx);
    bool isTransmitting() const { return m_transmitting; }
    // For tests: the interval currently in force.
    int  pollIntervalMs() const { return m_pollTimer.interval(); }

    static constexpr int kPollTxMs = 16;    // ~60 Hz
    static constexpr int kPollRxMs = 250;   // 4 Hz

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    void stateUpdated(const QMap<QString, QString>& kvs);
    void statusUpdated(const QMap<QString, QString>& kvs);
    // Operator-facing alert from the tuner ("LOW RF POWER" when a tune is
    // asked for with too little drive to measure). Empty text means the
    // tuner has cleared it, which it does on its own a few seconds later.
    // Broadcast to every connected client, not just the one that acted.
    void alertChanged(const QString& text);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError(QAbstractSocket::SocketError error);
    void pollStatus();

private:
    void applyPollRateFor(const QMap<QString, QString>& kvs);
    void processLine(const QString& line);

    QTcpSocket m_socket;
    QTimer     m_pollTimer;       // interval follows m_transmitting
    bool       m_transmitting{false};
    // One status poll in flight at a time. The transmit interval assumes the
    // round trip fits inside it; on a congested LAN it may not, and an
    // unconditional write would queue requests the device answers late and
    // we never asked for. Self-limiting instead: skip a tick while one is
    // outstanding, and give up on it after kPollStaleMs so a dropped reply
    // cannot wedge polling for good.
    bool          m_pollInFlight{false};
    QElapsedTimer m_pollSent;
    static constexpr int kPollStaleMs = 1000;
    QTimer     m_reconnectTimer;
    QByteArray m_readBuf;
    quint32    m_seq{0};
    bool       m_connected{false};
    bool       m_gotVersion{false};
    bool       m_autoReconnect{false};
    bool       m_deliberateDisconnect{false};
    QString    m_version;
    QString    m_lastHost;
    quint16    m_lastPort{9010};
};

} // namespace AetherSDR
