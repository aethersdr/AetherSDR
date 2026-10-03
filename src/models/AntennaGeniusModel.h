#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QHostAddress>
#include <QHash>
#include <QByteArray>
#include <QList>
#include <functional>

class QUdpSocket;
class QTcpSocket;
class QTimer;

namespace AetherSDR {

// Information about a discovered Antenna Genius device on the network.
struct AgDeviceInfo {
    QString   name;
    QString   serial;
    QString   version;
    QHostAddress ip;
    QString   host;       // Manual DNS name; empty for discovered devices.
    quint16   port{9007};
    int       radioPorts{2};
    int       antennaPorts{8};
    int       webPort{0};  // web UI port (0 = unknown, use SS_ManualPort fallback)
    QString   mode;       // "master" / "slave"
};

// Information about a single antenna port on the device.
struct AgAntennaInfo {
    int     id{0};
    QString name;
    quint16 txBandMask{0};
    quint16 rxBandMask{0};
    quint16 inbandMask{0};
};

// Information about a band definition on the device.
struct AgBandInfo {
    int     id{0};
    QString name;
    double  freqStartMhz{0.0};
    double  freqStopMhz{0.0};
};

// Current status of a radio port (A or B) on the Antenna Genius.
struct AgPortStatus {
    int     portId{0};
    bool    autoMode{true};
    QString source;         // "AUTO" or "MANUAL"
    int     band{0};
    int     rxAntenna{0};
    int     txAntenna{0};
    bool    transmitting{false};
    bool    inhibited{false};
};

// 4O3A Antenna Genius over TCP. Discovery: UDP broadcast on 9007
// ("AG ip=... port=..."). Control: TCP to device:9007.
//   Command:  C<seq>|<command>\r\n
//   Response: R<seq>|<hex_code>|<body>   (R<seq>|| ends a multi-line response)
//   Status:   S0|<object> key=val ...     (async after `sub port all` / `sub relay`)
// Commands used: antenna list, band list, port get <id>,
// port set <id> rxant=<n> txant=<n>, sub port all, sub relay, ping (keep-alive).
class AntennaGeniusModel : public QObject {
    Q_OBJECT

public:
    explicit AntennaGeniusModel(QObject* parent = nullptr);
    ~AntennaGeniusModel() override;

    // Start/stop UDP discovery listener.
    void startDiscovery();
    void stopDiscovery();

    // Connect to a specific device (by IP:port from discovery).
    void connectToDevice(const AgDeviceInfo& info);
    // Connect directly by IP address (for remote/manual connections).
    void connectToAddress(const QHostAddress& ip, quint16 port);
    void connectToAddress(const QString& host, quint16 port);
    void disconnectFromDevice();

    void setAutoReconnect(bool on) { m_autoReconnect = on; }
    void setAuthCode(const QString& code);
    void setAuthCodeForAttempt(quint64 attempt, const QString& code,
                               bool credentialStoreUnavailable = false);
    void resetAuthBudgetFor(const AgDeviceInfo& info);

    // Getters
    bool isConnected()   const { return m_connected; }
    bool isConnecting() const;
    bool isAuthBlocked() const { return authBlockedForTarget(m_attemptEndpoint); }
    bool isAuthBlockedFor(const QString& host, quint16 port) const;
    bool isAuthBlockedFor(const AgDeviceInfo& info) const;
    bool isPresent()     const { return !m_discoveredDevices.isEmpty(); }
    static bool isShackSwitch(const AgDeviceInfo& info);
    QString peerAddress() const;
    quint16 peerPort() const;
    // The host asked for on the current attempt: a name or a literal address.
    // Saved codes key on it; see PeripheralAuthStore.
    QString attemptHost() const { return m_attemptHost; }
    const AgDeviceInfo& connectedDevice() const { return m_device; }

    QList<AgDeviceInfo>   discoveredDevices() const { return m_discoveredDevices; }
    QList<AgAntennaInfo>  antennas()          const { return m_antennas; }
    QList<AgBandInfo>     bands()             const { return m_bands; }
    AgPortStatus          portA()             const { return m_portA; }
    AgPortStatus          portB()             const { return m_portB; }
    int                   lastRadioBand()     const { return m_lastRadioBand; }

    // Select antenna for a port (portId: 1=A, 2=B, antennaId: 1–N).
    // Sets rxant to antennaId. Sets txant only if the antenna has TX
    // permission for the current band; otherwise keeps current txant.
    void selectAntenna(int portId, int antennaId);

    // Set auto-mode for a port.
    void setAutoMode(int portId, bool on);

    // Check if an antenna has TX or RX permission for a given band.
    bool canTxOnBand(int antennaId, int bandId) const;
    bool canRxOnBand(int antennaId, int bandId) const;

    // Effective band for a port: AG-reported band if nonzero,
    // otherwise client-side radio-frequency-derived band (port A only).
    int effectiveBand(int portId) const;

    // Notify the model of the radio's current frequency (MHz).
    // Matches against the AG band list and triggers recall if band changed.
    void setRadioFrequency(double freqMhz);

    // Get band name by ID.
    QString bandName(int bandId) const;

    // Get antenna name by ID.
    QString antennaName(int antennaId) const;

signals:
    void deviceDiscovered(const AgDeviceInfo& info);
    void deviceLost(const QString& serial);
    void connected();
    void disconnected();
    void authCodeRequired(quint64 attempt);
    void authCodeAccepted(const QString& code);
    void enteredAuthCodeDiscarded();
    void connectionError(const QString& msg);

    void antennasChanged();        // antenna list refreshed
    void bandsChanged();           // band list refreshed
    void portStatusChanged(int portId);  // port A or B status updated
    void presenceChanged(bool present);  // any device discovered/lost
    void radioBandChanged(int bandId);   // client-side band derived from radio freq
    void deviceInfoChanged();            // m_device fields updated from info get or beacon

private slots:
    void onDiscoveryDatagram();
    void onTcpConnected();
    void onTcpDisconnected();
    void onTcpReadyRead();
    void onTcpError();
    void onKeepAlive();

private:
    friend struct AntennaGeniusModelTestAccess;
    Q_INVOKABLE void processTcpBytes(const QByteArray& bytes); // injected transport test seam
    Q_INVOKABLE void beginAttempt(); // same reset used before a real TCP connect
    Q_INVOKABLE void beginAttemptAt(const QString& host, quint16 port);
    Q_INVOKABLE void onAuthTimeout();
    // Send a command, returns sequence number.
    int sendCommand(const QString& cmd);

    // Apply parsed `info get` key=value fields to m_device.
    // Shared by the inline body-parse path and the terminator path.
    void applyDeviceInfo(const QString& line);

    // Process a received line from TCP.
    void processLine(const QString& line);
    void processResponse(int seq, int code, const QString& body);
    void processStatus(const QString& body);
    bool authBlockedForTarget(const QString& target) const;
    int recordAuthFailure();
    void completePrologue();
    void sendAuthentication();
    void failAuthentication(const QString& reason, bool blockReconnect = true);

    // Parse helpers
    static QMap<QString, QString> parseKeyValues(const QString& text);
    AgPortStatus parsePortStatus(const QString& text) const;

    // Band→antenna memory (persisted in AppSettings XML).
    void saveBandAntenna(int portId, int bandId, int antennaId);
    int  recallBandAntenna(int portId, int bandId) const;
    void onBandChanged(int portId, int oldBand, int oldAnt, int newBand);

    // Initialization sequence after TCP connect.
    void runInitSequence();

    // UDP discovery
    QUdpSocket* m_udpSocket{nullptr};
    QList<AgDeviceInfo> m_discoveredDevices;
    QTimer* m_discoveryTimeout{nullptr};  // prune stale devices
    QTimer* m_retryTimer{nullptr};        // retry bind if port busy

    // TCP connection
    QTcpSocket* m_tcpSocket{nullptr};
    std::function<void(const QByteArray&)> m_authCommandWriter;
    AgDeviceInfo m_device;
    bool m_connected{false};
    bool m_gotPrologue{false};
    bool m_authPending{false};
    bool m_waitingForAuthCode{false};
    QHash<QString, int> m_authFailuresByTarget;
    quint64 m_authAttempt{0};
    QString m_authCode;
    bool m_userAuthCode{false};
    QString m_attemptEndpoint;
    QString m_attemptHost;
    QString m_userAuthEndpoint;
    bool m_authCloseReported{false};
    QTimer* m_authTimer{nullptr};
    QString m_lineBuffer;

    // Command sequencing
    int m_nextSeq{1};

    // Pending multi-line responses: seq → (command, accumulated lines)
    struct PendingResponse {
        QString command;
        QStringList lines;
    };
    QMap<int, PendingResponse> m_pending;

    // State
    QList<AgAntennaInfo> m_antennas;
    QList<AgBandInfo>    m_bands;
    AgPortStatus         m_portA;
    AgPortStatus         m_portB;

    // Previous band per port (for detecting band changes)
    int m_prevBandA{-1};
    int m_prevBandB{-1};

    // Last band detected from radio frequency (for client-side band tracking)
    int m_lastRadioBand{0};

    // Cache of last radio frequency (MHz) — reprocessed when bands load.
    double m_lastRadioFreqMhz{0.0};

    // Keep-alive
    QTimer* m_keepAlive{nullptr};

    // Auto-reconnect
    bool    m_autoReconnect{false};
    bool    m_deliberateDisconnect{false};
    QTimer* m_reconnectTimer{nullptr};

    // Track init commands
    int m_seqAntennaList{0};
    int m_seqBandList{0};
    int m_seqPortA{0};
    int m_seqPortB{0};
    int m_seqSubPort{0};
    int m_seqSubRelay{0};
    int m_seqInfo{-1};
    QTimer* m_initWatchdog{nullptr};  // falls back to runInitSequence() if info get does not answer
};

} // namespace AetherSDR
