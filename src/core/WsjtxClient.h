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

// Live Rx/Tx state from one WSJT-X instance's Status (type 1) message,
// resolved to absolute frequencies (#4526). WSJT-X reports the dial
// frequency plus separate Rx/Tx AUDIO offsets (Hz within the passband);
// this is dial + offset for each, matching the addition parseDecode() has
// always used for spot placement — WSJT-X's audio-offset convention adds
// unconditionally regardless of the reported sideband, so there is no
// LSB/USB special case here.
struct WsjtxStatus {
    QString id;
    double  rxFreqHz{0.0};
    double  txFreqHz{0.0};
    bool    transmitting{false};
    QString mode;
};

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
    // Was (id, dialFreqHz, mode); widened to WsjtxStatus for the Rx/Tx
    // frequency overlay (#4526) rather than growing the signal's argument
    // list further. Previously emitted with no consumer connected — see
    // MainWindow_Spots.cpp for the wiring this enables.
    void statusReceived(const WsjtxStatus& status);

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
