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
    double  dialFreqHz{0.0};  // raw radio dial frequency, no audio offset
    double  rxFreqHz{0.0};    // dial + Rx audio offset (full RF)
    double  txFreqHz{0.0};    // dial + Tx audio offset (full RF)
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
    // Close (type 6): this instance is exiting cleanly. Lets a consumer drop
    // its marker immediately instead of waiting out an activity-timeout
    // sweep (RFC #4526 review item 3).
    void instanceClosed(const QString& id);
    // Heartbeat (type 0, ~15s, no frequency data): evidence this instance is
    // still alive even during a quiet spell in Status traffic — e.g. WSJT-X
    // decoding is running but Monitor is off, so nothing new to report.
    // Refreshes an activity timeout without asserting a frequency (RFC #4526
    // review item 4).
    void instanceHeartbeat(const QString& id);

private slots:
    void onReadyRead();

private:
    static constexpr quint32 WsjtxMagic = 0xadbccbda;

    void parseMessage(const QByteArray& data);
    void parseHeartbeat(QDataStream& ds);
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

// WsjtxClient lives on the SpotClients worker thread (#1929); statusReceived()
// crosses to the GUI thread as a queued connection, so WsjtxStatus needs a
// registered metatype or QObject::connect fails at RUNTIME with an
// unregistered-type warning and the signal silently never arrives — no
// compile error, no crash, just a marker that never appears (RFC #4526
// review). Declared here, at file scope, matching the house pattern
// (RecordStartDecision in QsoRecorder.h, etc.).
Q_DECLARE_METATYPE(AetherSDR::WsjtxStatus)
