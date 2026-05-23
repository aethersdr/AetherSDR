#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QFile>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <atomic>
#include "DxClusterClient.h"  // for DxSpot

namespace MasterSDR {

// FreeDV Reporter spot client — connects to qso.freedv.org via WebSocket
// with manual Engine.IO v4 / Socket.IO v4 framing. Tracks station state
// (callsign, frequency, grid, mode) and emits spotReceived() for each
// rx_report event (station heard another station).
//
// View-only by default. Call enableReporting() when RADE is active to
// register as a full participant: transmits freq_change, tx_report, and
// periodic rx_report (SNR) events to the server.
class FreeDvClient : public QObject {
    Q_OBJECT

public:
    explicit FreeDvClient(QObject* parent = nullptr);
    ~FreeDvClient() override;

    void startConnection();
    void stopConnection();
    bool isConnected() const { return m_connected.load(); }

    QString logFilePath() const;

signals:
    void started();
    void stopped();
    void connectionError(const QString& error);
    void spotReceived(const DxSpot& spot);
    void rawLineReceived(const QString& line);

public slots:
    // Defer QWebSocket + timer construction to the worker thread (#1929) —
    // see DxClusterClient::initialize().
    void initialize();

    // Station reporting — call from RADE activate/deactivate paths.
    // All methods are safe to call from any thread via queued connection.
    void enableReporting(const QString& callsign, const QString& grid,
                         const QString& message, const QString& softwareVersion,
                         double freqMhz);
    void disableReporting();

    void reportFreqChange(double freqMhz);
    void reportTxState(bool transmitting);
    void updateMessage(const QString& message);

    // Feed live SNR from RADEEngine::snrChanged — throttled to 1 s by internal timer.
    void updateRxSnr(float snrDb);
    void updateRxSynced(bool synced);

    // Feed decoded far-end callsign from RADEEngine::eooCallsignReceived.
    // Included in the next rx_report sent to qso.freedv.org.
    void updateRxCallsign(const QString& callsign);

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessage(const QString& message);
    void onWsError(QAbstractSocket::SocketError err);
    void onReconnectTimer();
    void onSnrTimer();

private:
    // Per-station state tracked by Socket.IO session ID
    struct StationInfo {
        QString callsign;
        QString gridSquare;
        double  freqMhz{0.0};
        QString mode;
        bool    rxOnly{false};
    };

    void handleEngineIO(const QString& raw);
    void handleSocketIO(const QString& payload);
    void handleEvent(const QString& eventName, const QJsonObject& data);
    void onNewConnection(const QJsonObject& data);
    void onFreqChange(const QJsonObject& data);
    void onRxReport(const QJsonObject& data);
    void onTxReport(const QJsonObject& data);
    void onRemoveConnection(const QJsonObject& data);
    void onBulkUpdate(const QJsonArray& pairs);

    void sendEvent(const QString& name, const QJsonObject& data);
    void sendInitialFreqChange();

    QWebSocket*  m_ws{nullptr};
    QTimer*      m_pingTimer{nullptr};
    QTimer*      m_reconnectTimer{nullptr};
    QTimer*      m_snrTimer{nullptr};
    QFile        m_logFile;

    QHash<QString, StationInfo> m_stations;  // sid → station info

    // Station reporting state
    bool    m_reportingEnabled{false};
    bool    m_authNeedsRefresh{false};  // reconnect in progress for auth change
    QString m_myCallsign;
    QString m_myGrid;
    QString m_mySoftwareVersion;
    QString m_myMessage;
    double  m_myFreqMhz{0.0};
    float   m_lastSnr{-99.0f};
    bool    m_radeSynced{false};
    QString m_rxCallsign;

    std::atomic<bool> m_connected{false};
    bool    m_intentionalDisconnect{false};
    int     m_reconnectAttempts{0};
    int     m_pingIntervalMs{25000};

    static constexpr const char* WsUrl =
        "wss://qso.freedv.org/socket.io/?EIO=4&transport=websocket";
    static constexpr int MaxReconnectDelayMs     = 60000;
    static constexpr int InitialReconnectDelayMs  = 5000;
};

} // namespace MasterSDR
