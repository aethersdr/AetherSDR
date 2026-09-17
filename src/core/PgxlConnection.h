#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QMap>
#include <QString>

namespace AetherSDR {

// Direct TCP connection to a 4O3A Power Genius XL on port 9008.
// Same protocol as TgxlConnection (C/R/S/V/M message format).
// Provides PGXL status telemetry: state, power, SWR, current,
// temperature, mains voltage, band, bias mode, fan mode.
class PgxlConnection : public QObject {
    Q_OBJECT

public:
    explicit PgxlConnection(QObject* parent = nullptr);

    bool isConnected() const { return m_connected; }
    QString version() const { return m_version; }
    QString peerAddress() const { return m_socket.peerAddress().toString(); }
    quint16 peerPort() const { return m_socket.peerPort(); }

    void connectToPgxl(const QString& host, quint16 port = 9008);
    void disconnect();

    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    quint32 sendCommand(const QString& cmd);

signals:
    void connected();
    void disconnected();
    void statusUpdated(const QMap<QString, QString>& kvs);
    // The reply to `setup read` — the amplifier's stored configuration
    // (nickname, ledintens, txdelay, inactivity-timeout, authcode).
    //
    // It arrives as an ordinary R frame of key/value pairs, indistinguishable
    // from a status reply by shape alone, so it is matched by the sequence
    // number of the `setup read` that asked for it. Without that it would be
    // published as a status frame carrying none of the fields a status frame
    // carries.
    //
    // Needed because `setup` WRITES take the whole group at once — the vendor
    // utility sends `setup nickname=… meffa=… ledintens=… fanmode=… authcode=`
    // as one line — so changing any one of them means knowing the rest.
    void setupRead(const QMap<QString, QString>& kvs);
    // The amplifier refused a command: `R<seq>|<code>|` with a non-zero code
    // and an empty body. 50000013 is a bad parameter (a `setup` carrying a
    // value the amplifier will not take), 50000015 an unknown command.
    // Emitted so a refusal is visible rather than being read as an empty
    // status frame and dropped.
    void commandRefused(quint32 seq, const QString& code);
    // Operator-facing alert, empty text meaning the amplifier has cleared it.
    // Same `M|<text>` frame the tuner uses — the two devices share a protocol
    // and a vendor. Broadcast to every connected client, not only the one
    // that acted.
    //
    // No PGXL alert has actually been captured: the frame is handled because
    // the framing is shared and doing so costs nothing, not because one was
    // observed. See pgxl_direct_protocol_test.
    void alertChanged(const QString& text);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError(QAbstractSocket::SocketError error);
    void pollStatus();

private:
    void processLine(const QString& line);

    QTcpSocket m_socket;
    QTimer     m_pollTimer;
    QTimer     m_reconnectTimer;
    QByteArray m_readBuf;
    quint32    m_seq{0};
    // Sequence number of the outstanding `setup read`, 0 when none. See
    // setupRead().
    quint32    m_setupReadSeq{0};
    bool       m_connected{false};
    bool       m_gotVersion{false};
    bool       m_autoReconnect{false};
    bool       m_deliberateDisconnect{false};
    QString    m_version;
    QString    m_lastHost;
    quint16    m_lastPort{9008};
};

} // namespace AetherSDR
