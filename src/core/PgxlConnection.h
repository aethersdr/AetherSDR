#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QMap>
#include <QString>

namespace AetherSDR {

// Direct TCP connection to a 4O3A Power Genius XL on port 9008.
// Same protocol as TgxlConnection (C/R/S/V message format).
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
    bool       m_connected{false};
    bool       m_gotVersion{false};
    bool       m_autoReconnect{false};
    bool       m_deliberateDisconnect{false};
    QString    m_version;
    QString    m_lastHost;
    quint16    m_lastPort{9008};
};

} // namespace AetherSDR
