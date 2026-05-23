#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QHostAddress>
#include <QList>

class QUdpSocket;
class QTcpSocket;
class QTimer;

namespace MasterSDR {

// Information about a discovered Antenna Genius device on the network.
struct AgDeviceInfo {
    QString   name;
    QString   serial;
    QString   version;
    QHostAddress ip;
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

// State model for a 4O3A Antenna Genius connected via TCP/IP.
//
// Discovery: UDP broadcast listener on port 9007 (device sends "AG ip=... port=... ...")
// Control:   TCP connection to device:9007, command/response protocol similar to SmartSDR
//
// Protocol summary:
//   Commands:  C<seq>|<command>\r\n
//   Responses: R<seq>|<hex_code>|<body>    (R<seq>|| = end of multi-line response)
//   Status:    S0|<object> key=val ...      (async push after sub port all / sub relay)
//
// Commands used:
//   antenna list        — enumerate antenna ports
//   band list           — enumerate band definitions
//   port get <id>       — get current port status
//   port set <id> rxant=<n> txant=<n>  — select antenna for a port
//   sub port all        — subscribe to port status changes
//   sub relay           — subscribe to relay changes
//   ping                — keep-alive heartbeat
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
    void disconnectFromDevice();

    // Getters
    bool isConnected()   const { return m_connected; }
    bool isConnecting()  const { return m_tcpSocket != nullptr && !m_connected; }
    bool isPresent()     const { return !m_discoveredDevices.isEmpty(); }
    static bool isShackSwitch(const AgDeviceInfo& info);
    QString peerAddress() const;
    quint16 peerPort() const;
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
    void connectionError(const QString& msg);

    void antennasChanged();        // antenna list refreshed
    void bandsChanged();           // band list refreshed
    void portStatusChanged(int portId);  // port A or B status updated
    void presenceChanged(bool present);  // any device discovered/lost
    void radioBandChanged(int bandId);   // client-side band derived from radio freq

private slots:
    void onDiscoveryDatagram();
    void onTcpConnected();
    void onTcpDisconnected();
    void onTcpReadyRead();
    void onTcpError();
    void onKeepAlive();

private:
    // Send a command, returns sequence number.
    int sendCommand(const QString& cmd);

    // Process a received line from TCP.
    void processLine(const QString& line);
    void processResponse(int seq, int code, const QString& body);
    void processStatus(const QString& body);

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
    AgDeviceInfo m_device;
    bool m_connected{false};
    bool m_gotPrologue{false};
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

    // Track init commands
    int m_seqAntennaList{0};
    int m_seqBandList{0};
    int m_seqPortA{0};
    int m_seqPortB{0};
    int m_seqSubPort{0};
    int m_seqSubRelay{0};
};

} // namespace MasterSDR
