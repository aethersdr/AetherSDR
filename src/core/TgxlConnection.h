#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QMap>
#include <QString>

namespace MasterSDR {

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

    // Manual relay adjustment: relay 0=C1, 1=L, 2=C2; direction +1 or -1
    void adjustRelay(int relay, int direction);

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
    QByteArray m_readBuf;
    quint32    m_seq{0};
    bool       m_connected{false};
    bool       m_gotVersion{false};
    QString    m_version;
};

} // namespace MasterSDR
