#pragma once
#include "Kxpa100Types.h"
#include <QObject>
#include <QElapsedTimer>
#include <QMap>
#include <QQueue>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>

class QTcpSocket;
class QSerialPort;
namespace AetherSDR::Kxpa100 { class FrameParser; }
namespace AetherSDR {
class Kxpa100Model : public QObject {
    Q_OBJECT
public:
    explicit Kxpa100Model(QObject* parent = nullptr);
    ~Kxpa100Model() override;
    void connectToHost(const QString& host, quint16 port);
    void connectSerial(const QString& portName, int baudRate = 38400);
    void disconnectDevice();
    void setAutoReconnect(bool enabled) { m_autoReconnect = enabled; }
    bool isConnected() const { return m_connected; }
    bool isConnecting() const { return m_connecting; }
    bool isDetected() const { return m_detected; }
    bool isResponding() const;
    bool isNetworkConnection() const { return m_network; }
    QString description() const;
    const Kxpa100::Status& status() const { return m_status; }
    qint64 fieldAgeMs(const QString& command) const;
    QString unavailableReason(bool writing = false, bool tuner = false) const;
    bool tunerEligible() const;
    bool amplifierMeterEligible() const;
    bool operationBusy() const { return !m_operation.isEmpty(); }
    QString operationName() const { return m_operation; }
    bool setOperate(bool operate);
    bool setMode(Kxpa100::Mode mode);
    bool setAntenna(int antenna);
    bool setCapacitorMask(int mask);
    bool setInductorMask(int mask);
    bool setCapacitorSide(Kxpa100::CapacitorSide side);
    bool setBypassed(bool bypassed);
    bool setAttenuator(bool enabled);
    bool clearFault();
    bool startTune(std::function<bool()> permit = {});
    bool cancelTune();
    void cancelTuneAfterPendingOperation();
    bool readMatchStatus();
    bool memorize(int frequencyKhz, std::function<bool()> permit = {});
    bool recallFrequency(int frequencyKhz);
    bool setBand(int band);
    const std::optional<Kxpa100::Configuration>& configuration() const { return m_configuration; }
    bool readConfiguration();
    bool applyConfiguration(const Kxpa100::Configuration& configuration);
    bool readBackup();
    bool restoreBackup(const Kxpa100::Backup& backup, bool confirmed);
    const Kxpa100::Backup& backup() const { return m_backup; }
    static QByteArray encodeBackupFile(const Kxpa100::Backup& backup);
    static std::optional<Kxpa100::Backup> decodeBackupFile(const QByteArray& data, QString* error = nullptr);
    static bool saveBackupFile(const QString& path, const Kxpa100::Backup& backup, QString* error = nullptr);
    bool readFaultHistory();
    const QList<Kxpa100::FaultRecord>& faults() const { return m_faults; }
    static QByteArray faultsCsv(const QList<Kxpa100::FaultRecord>& faults);
    bool inspectMemories(int frequencyKhz);
    const Kxpa100::MemoryBin& memories() const { return m_memories; }
    bool eraseMemories(int band, int antenna, bool confirmed);
    bool factoryReset(bool confirmed);
    void cancelOperation();
    void setVerbose(bool enabled) { m_verbose = enabled; }
    bool verbose() const { return m_verbose; }
    QString debugText() const { return m_debug.join(QLatin1Char('\n')); }
    void clearDebug() { m_debug.clear(); emit debugChanged(); }
signals:
    void associationChanged();
    void connected();
    void disconnected();
    void connectionFailed(const QString& reason);
    void detectedChanged(bool detected);
    void statusUpdated(const AetherSDR::Kxpa100::Status& status);
    void telemetryReceived(const QString& command, const AetherSDR::Kxpa100::Status& status);
    void availabilityChanged();
    void diagnostic(const QString& line);
    void debugChanged();
    void operationBusyChanged(bool busy);
    void operationProgress(int completed, int total);
    void operationFinished(const QString& message);
    void operationFailed(const QString& reason);
    void configurationRead(const AetherSDR::Kxpa100::Configuration& configuration);
    void backupRead(const AetherSDR::Kxpa100::Backup& backup);
    void faultHistoryRead(const QList<AetherSDR::Kxpa100::FaultRecord>& records);
    void memoriesRead(const AetherSDR::Kxpa100::MemoryBin& bin);
private:
    friend class Kxpa100ModelTest;
    struct Request {
        QByteArray wire, telemetryKey;
        std::function<bool(const QByteArray&)> matches;
        std::function<void(const QByteArray&)> complete;
        int timeoutMs = 2000;
        bool mutation = false, requiresIdle = true;
    };
    void transportOpened();
    void transportClosed(const QString& reason = {});
    void reconnect();
    void receiveBytes(const QByteArray& bytes);
    void receiveFrame(const QByteArray& frame);
    void tick();
    bool fresh(const QString& key, int maxAge = 5000) const;
    bool idle() const;
    bool write(const QByteArray& bytes);
    void enqueue(Request request);
    void query(const QByteArray& wire, std::function<void()> complete = {});
    void queueIdentity();
    bool begin(const QString& name, bool writing, bool tuner = false);
    void finish(const QString& message);
    void fail(const QString& reason);
    void report(const QString& text, bool polling = false);
    void preflight(std::function<void()> ready, bool requireIdle = true);
    void readMatch(std::function<void()> complete);
    bool requestValue(const QByteArray& setter, const QByteArray& getter,
                      std::function<bool(const Kxpa100::Status&)> verify, const QString& name, bool tuner = false);
    void settle(const QByteArray& setter, const QByteArray& getter,
                std::function<bool(const Kxpa100::Status&)> verify, bool requireIdle = true);
    void pollSettle();
    void confirmSettle();
    void requestMemory(int frequencyKhz, const QByteArray& prefix, std::function<void()> complete);
    void applyNextSetting();
    void readRegion(bool external, int address, int end, std::function<void(QByteArray)> complete);
    void readRegionBlock();
    void restoreBlock();
    void restoreNextRegion();
    void restartAfterMaintenance(const QString& message);

    std::unique_ptr<Kxpa100::FrameParser> m_parser;
    std::unique_ptr<QTcpSocket> m_socket;
#ifdef HAVE_SERIALPORT
    std::unique_ptr<QSerialPort> m_serial;
#endif
    QElapsedTimer m_clock;
    QTimer m_timer, m_reconnectTimer;
    std::function<qint64()> m_now;
    std::function<bool(const QByteArray&)> m_write;
    QQueue<Request> m_queue;
    std::optional<Request> m_active;
    quint64 m_generation = 0;
    qint64 m_sentAt = 0, m_lastReply = -10000, m_nextPoll = 0, m_connectStarted = 0;
    int m_pollIndex = 0;
    bool m_connected = false, m_connecting = false, m_detected = false, m_closing = false;
    bool m_network = true, m_autoReconnect = false, m_deliberateDisconnect = false;
    bool m_verbose = false, m_deferredTuneCancel = false, m_tuneEnded = false;
    QString m_host, m_portName;
    quint16 m_port = 0;
    int m_baud = 38400;
    Kxpa100::Status m_status;
    QMap<QString, qint64> m_received;
    QStringList m_debug;
    QString m_operation, m_operationSerial, m_operationFirmware;
    bool m_operationWriting = false;
    int m_progress = 0, m_total = 0;
    QByteArray m_settleGetter;
    std::function<bool(const Kxpa100::Status&)> m_settleVerify;
    std::function<bool()> m_tunePermit;
    qint64 m_settleDeadline = 0, m_settleNext = 0;
    bool m_settleRequiresIdle = true;
    std::optional<Kxpa100::Configuration> m_configuration;
    QQueue<QPair<int, int>> m_edits;
    Kxpa100::Backup m_backup, m_pendingBackup, m_restore;
    QList<Kxpa100::FaultRecord> m_faults;
    Kxpa100::MemoryBin m_memories;
    bool m_regionExternal = false;
    int m_regionAddress = 0, m_regionEnd = 0;
    QByteArray m_regionData;
    std::function<void(QByteArray)> m_regionComplete;
    bool m_restoreExternal = false;
    int m_restoreAddress = 0;
};
}
