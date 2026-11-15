#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QIODevice>
#include <QTcpSocket>

#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif

#include "Kpa500Protocol.h"

namespace AetherSDR {

// Peripheral transport for an Elecraft KPA500 linear amplifier connected over
// RS-232/USB serial or a raw TCP serial bridge. A standalone device with no FlexRadio awareness — a
// peripheral(kpa500) accessory alongside AcomConnection/SpeConnection, not an
// IRadioBackend implementor.
//
// The KPA500 speaks a text-based ^CMD[data]; protocol (see Kpa500Protocol.h)
// over either transport. This class owns the poll loop (fast every 200 ms,
// slow rotated every 500 ms, one-shot on connect) and emits statusUpdated for
// every reply that changes the snapshot.
//
// Default serial rate is 4800 baud as per the KPA500 spec; configurable via
// RadioSetupDialog. Reconnect is automatic when the port disappears and
// reappears (e.g. USB detach/reattach).
//
// Principle II: no control method latches its own state. setOperate() sends
// the command; the applet repaints when the poll reply lands.
class Kpa500Connection : public QObject {
    Q_OBJECT

public:
    explicit Kpa500Connection(QObject* parent = nullptr);
    ~Kpa500Connection() override;

    bool isConnected() const { return m_connected; }
    bool isConnecting() const { return m_connecting; }
    bool isNetworkConnection() const { return m_network; }
    bool isResponding() const { return m_responding; }
    // Other replies can stay live after power telemetry expires.
    bool hasLiveTxMeters() const {
        return m_connected && m_responding && m_status.powerOn == true
            && m_status.operate == true && m_status.forwardPowerW.has_value();
    }
    // A valid reply has identified the amp during this open transport session.
    bool isDetected() const { return m_detected; }
    // Serial endpoint/rate or raw TCP endpoint; bridge baud is server-configured.
    QString description() const;

#ifdef HAVE_SERIALPORT
    // Opens portName at baudRate (default 4800, per KPA500 spec §^BRP).
    // 8N1, no handshake. No-op if already connected or connecting.
    void connectSerial(const QString& portName, int baudRate = 4800);
#endif

    // Raw TCP only, without Telnet/RFC2217 negotiation. No-op while connected
    // or connecting; the serial bridge owns its baud and line settings.
    void connectToHost(const QString& host, quint16 port);

    void disconnect();
    void setAutoReconnect(bool on);

    // Commands — no-ops when not connected (Principle II: send and let poll confirm).
    void setOperate(bool on);
    void clearFault();
    // Set fan minimum speed (0 = off, 6 = high). Clamped to [0, 6].
    void setFanSpeed(int n);
    void setPower(bool on);
    void setSpeaker(bool on);
    void setFollowTx(bool on);
    void setTxContext(int band, const QString& source, bool transmitting);
    void setTransmitting(bool transmitting);
    void requestBand(int band);
    QString bandTrackingText() const;


    bool maintenanceBusy() const { return m_maintenanceOperation != MaintenanceOperation::None; }
    QString maintenanceUnavailableReason(bool writing) const;
    void readConfiguration();
    void readFaultHistory();
    void readPowerOnBanner();
    void writePowerOnBanner(const QString& banner);
    void restoreConfiguration(const QByteArray& data, const QString& expectedSerial,
                              const QString& expectedFirmware);
    void cancelMaintenance();

    const Kpa500::Status& lastStatus() const { return m_status; }

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    void statusUpdated(const AetherSDR::Kpa500::Status& status);
    // Transport is up but the amplifier has gone silent (or resumed). A USB-
    // serial link can outlive the amp being switched off, so this — not
    // disconnected() — is the "amp went away" signal for that topology.
    void respondingChanged(bool responding);
    // Retained through power-off and silence so the wake control stays accessible.
    void detectedChanged(bool detected);
    void telemetryReceived(const QString& command, const AetherSDR::Kpa500::Status& status);
    void diagnostic(const QString& line);
    void bandTrackingChanged(const QString& text);
    void maintenanceBusyChanged(bool busy);
    void maintenanceAvailabilityChanged();
    void maintenanceProgress(int done, int total);
    void configurationRead(const QByteArray& data);
    void faultHistoryRead(const QByteArray& data);
    void powerOnBannerRead(const QString& banner);
    void maintenanceFinished(const QString& message);
    void maintenanceFailed(const QString& reason);


private slots:
    void onReadyRead();
#ifdef HAVE_SERIALPORT
    void onSerialError(QSerialPort::SerialPortError error);
#endif

private:
    friend class Kpa500ConnectionTest;

    void onTransportUp();
    void onTransportDown(const QString& reason = {});
    void onTransportError(const QString& reason);
    void onConnectTimeout();
    void retryConnection();
    void fastPollTick();
    void slowPollTick();
    void sendRaw(const QByteArray& bytes);
    void armReconnect();
    void setResponding(bool responding);
    void receiveBytes(const QByteArray& bytes);
    void updateBand();
    void report(const QString& line);
    void expireTelemetry();
    void enterPowerCheckMode();
    bool busy() const;

    enum class MaintenanceOperation { None, ConfigurationRead, FaultHistoryRead, BannerRead,
                                      BannerWrite, ConfigurationRestore };
    enum class MaintenancePhase { None, PreflightIdentity, PreflightPower, PreflightStandby, PreflightRf,
                                  Reading, Writing, Verifying };
    bool beginMaintenance(MaintenanceOperation operation, int startAddress, int length,
                          const QByteArray& writeData = {});
    bool maintenanceWriting() const;
    void sendMaintenanceRequest(const QByteArray& request);
    void sendMaintenanceBlock();
    void startMaintenancePreflight();
    void handleMaintenanceStatus(const QString& command);
    void handleMaintenanceReply(const QString& command, const QString& argument);
    void onMaintenanceTimeout();
    void finishMaintenance();
    void failMaintenance(const QString& reason);
    void resetMaintenance();



#ifdef HAVE_SERIALPORT
    QSerialPort m_serial;
#endif

    QTcpSocket m_socket;
    QIODevice* m_device{nullptr};
    bool m_network{false};
    bool m_connecting{false};
    QString m_lastHost;
    quint16 m_lastTcpPort{0};
    QTimer m_connectTimer;

    MaintenanceOperation m_maintenanceOperation{MaintenanceOperation::None};
    MaintenancePhase m_maintenancePhase{MaintenancePhase::None};
    QTimer m_maintenanceTimer;
    QByteArray m_maintenanceRequest;
    QByteArray m_maintenanceData;
    QByteArray m_maintenanceWriteData;
    QString m_maintenanceSerial;
    QString m_maintenanceFirmware;
    int m_maintenanceStartAddress{0};
    int m_maintenanceLength{0};
    int m_maintenanceOffset{0};
    int m_maintenanceExpectedLength{0};
    int m_maintenanceReadRetries{0};
    bool m_maintenanceWriteStarted{false};
    static constexpr int kMaintenanceTimeoutMs = 2000;

    QElapsedTimer m_clock;
    QHash<QString, qint64> m_received;
    bool m_followTx{true};
    bool m_transmitting{false};
    bool m_powerOffRequested{false};
    bool m_powerCheckOnly{false};
    bool m_powerCheckPending{false};
    qint64 m_lastPowerCheckAt{0};
    int m_wakeTicks{0};
    int m_txBand{-1};
    int m_manualBand{-1};
    int m_sentBand{-1};
    int m_failedBand{-1};
    qint64 m_bandSentAt{0};
    QString m_txSource;
    QString m_trackingText;
    Kpa500::FrameParser m_parser;
    Kpa500::Status      m_status;

    QString m_lastPortName;
    int     m_lastBaudRate{4800};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};

    QTimer m_reconnectTimer;
    QTimer m_fastPollTimer;
    QTimer m_slowPollTimer;

    // Rotating index into slowPollCommands() — one command per slow tick.
    int m_slowIndex{0};
    // Rotating index into fastPollCommands() — member so reconnect resets it
    // and multiple instances don't share state.
    int m_fastIndex{0};
    // One-shot connect commands: sent in sequence after transport comes up,
    // one per fast-poll tick, before regular polling starts.
    int  m_connectCmdIndex{0};
    bool m_connectCmdsDone{false};

    // Poll-silence tracking — a few missed fast-poll replies are tolerated
    // (serial latency, amp busy) before flagging silence.
    bool m_statusSeenSinceFastTick{false};
    int  m_silentTicks{0};
    bool m_responding{false};
    bool m_detected{false};
    // ~3 s at 200 ms fast-poll cadence before flagging silence.
    static constexpr int kSilentTickLimit = 15;

    static constexpr int kFastPollMs  = 200;
    static constexpr int kSlowPollMs  = 500;
    static constexpr int kReconnectMs = 5000;
    static constexpr int kConnectTimeoutMs = 10000;
    static constexpr qint64 kMaxPendingWriteBytes = 4096;
    static constexpr int kPowerCheckMs = 5000;
};

}  // namespace AetherSDR
