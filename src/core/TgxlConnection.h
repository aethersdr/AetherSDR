#pragma once

#include <QObject>
#include <QTcpSocket>
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

    // Manual relay adjustment: relay 0=C1, 1=L, 2=C2. `steps` is a signed,
    // relative step count sent verbatim as the move= argument — the wire takes
    // any relative number, so a drag of twelve steps is one command rather
    // than twelve. Clamped to +/-kMaxRelayMove; 0 sends nothing.
    void adjustRelay(int relay, int steps);

    // Operate/standby over the direct channel: "operate set=1" brings the tuner
    // online, "operate set=0" puts it in standby. The radio-relayed
    // "tgxl set handle=<H> mode=" path needs a Flex handle, which a TGXL seen
    // only over port 9010 never has (#2250) — this is the direct equivalent.
    void setOperate(bool on);

    // Native autotune over the direct port-9010 channel. The TGXL drives
    // radio PTT via its hardware interlock cable, so no client-side keying
    // is required. Bypasses the firmware's `tgxl autotune` command path
    // (broken in firmware 4.2 — see issue tracker for "TUNE button on TGXL").
    void requestAutotune();

    // Send an arbitrary command to the TGXL (e.g. "activate ant=2")
    quint32 sendCommand(const QString& cmd);

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    void stateUpdated(const QMap<QString, QString>& kvs);
    void statusUpdated(const QMap<QString, QString>& kvs);
    // Reply to the "info" command sent on connect. Carries the device's fixed
    // capabilities — notably "3way=1" on the models with the antenna switch.
    void infoUpdated(const QMap<QString, QString>& kvs);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError(QAbstractSocket::SocketError error);
    void pollStatus();

private:
    void processLine(const QString& line);

    QTcpSocket m_socket;
    QTimer     m_pollTimer;       // 1/sec status poll
    QTimer     m_reconnectTimer;
    QByteArray m_readBuf;
    quint32    m_seq{0};
    quint32    m_seqInfo{0};      // seq of the outstanding "info" command
    // Verbatim logging of the first lines after each connect, so the wire's
    // actual key names can be read out of the log (see onReadyRead).
    static constexpr int kRxLogLines = 12;
    // Ceiling on one relay command. The relay range is 0-255, so a single move
    // never needs to be larger, and it keeps a runaway drag from sending
    // something absurd.
    static constexpr int kMaxRelayMove = 255;
    int        m_rxLogRemaining{kRxLogLines};
    bool       m_connected{false};
    bool       m_gotVersion{false};
    bool       m_autoReconnect{false};
    bool       m_deliberateDisconnect{false};
    QString    m_version;
    QString    m_lastHost;
    quint16    m_lastPort{9010};
};

} // namespace AetherSDR
