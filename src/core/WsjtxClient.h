#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QHostAddress>
#include <QFile>
#include <QString>
#include <atomic>
#include "DxClusterClient.h"  // for DxSpot
#include "WsjtxDialTracker.h"

namespace AetherSDR {

// WSJT-X UDP multicast client — listens for Decode messages (type 2)
// from WSJT-X and emits spotReceived() for each decoded station.
// Protocol: binary QDataStream on 224.0.0.1:2237 (default).
class WsjtxClient : public QObject {
    Q_OBJECT

public:
    explicit WsjtxClient(QObject* parent = nullptr);
    ~WsjtxClient() override;

    void startListening(const QString& address, quint16 port);
    void stopListening();
    bool isListening() const { return m_listening; }

    QString logFilePath() const;

public slots:
    // Defer socket construction to the worker thread (#1929) — see DxClusterClient::initialize().
    void initialize();

signals:
    void listening();
    void stopped();
    void spotReceived(const DxSpot& spot);
    void rawLineReceived(const QString& line);
    void statusReceived(const QString& id, double dialFreqHz, const QString& mode);

private slots:
    void onReadyRead();

private:
    static constexpr quint32 WsjtxMagic = 0xadbccbda;

    // QDataStream helpers — parse big-endian Qt-serialized types
    static bool readQString(QDataStream& ds, QString& out);
    static bool readBool(QDataStream& ds, bool& out);

    void parseMessage(const QByteArray& data);
    void parseStatus(QDataStream& ds);
    void parseDecode(QDataStream& ds);
    void parseClose(QDataStream& ds);
    QString extractCallsign(const QString& message) const;

    QUdpSocket* m_socket{nullptr};
    QFile       m_logFile;
    QHostAddress m_bindAddr;
    bool        m_isMulticast{false};
    quint16     m_port{2237};
    std::atomic<bool> m_listening{false};

    // Dial frequency from Status messages (type 1), kept PER INSTANCE ID so
    // two WSJT-X instances sharing this port each place their decodes on
    // their own band (#3595) — see WsjtxDialTracker.
    WsjtxDialTracker m_dialTracker;
};

} // namespace AetherSDR
